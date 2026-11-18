#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/uan-module.h"
#include "ns3/wifi-module.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace ns3;

namespace
{

constexpr uint16_t GatewayPort = 9000;
constexpr uint16_t GcsPort = 9001;
constexpr uint32_t PayloadBytes = 64;
constexpr double OutageStart = 10;
constexpr double OutageEnd = 15;
constexpr std::array<const char*, 3> Sources{"UUV1", "UUV2", "UAV"};

struct PacketRecord
{
    std::size_t source;
    int64_t tx = -1;
    int64_t gatewayRx = -1;
    int64_t forward = -1;
    int64_t gcsRx = -1;
    const char* outcome = "no_gateway_rx";
};

class CoastalScenario
{
  public:
    void Configure(double distance, double rate, double duration);
    void WriteResults(double duration, const std::string& prefix) const;

    bool outage = false;
    uint32_t bufferPackets = 0;

  private:
    void Send(uint32_t id);
    void ReceiveAtGateway(Ptr<Socket> socket);
    void ReceiveAtGcs(Ptr<Socket> socket);
    void Forward(Ptr<Packet> packet, uint32_t id);
    void Flush();

    Ptr<Socket> uanTx[2];
    Ptr<Socket> uanRx;
    Ptr<Socket> uavTx;
    Ptr<Socket> rfRx;
    Ptr<Socket> relayTx;
    Ptr<Socket> gcsRx;
    PacketSocketAddress acousticDestination[2];
    Ipv4Address gatewayAddress;
    Ipv4Address gcsAddress;
    std::vector<PacketRecord> records;
    std::deque<std::pair<Ptr<Packet>, uint32_t>> pending;
};

uint32_t
ReadId(Ptr<const Packet> packet)
{
    std::array<uint8_t, 4> bytes{};
    NS_ABORT_MSG_UNLESS(packet->GetSize() == PayloadBytes, "Unexpected payload size");
    packet->CopyData(bytes.data(), bytes.size());
    return (uint32_t{bytes[0]} << 24) | (uint32_t{bytes[1]} << 16) |
           (uint32_t{bytes[2]} << 8) | uint32_t{bytes[3]};
}

void
CoastalScenario::Configure(double distance, double rate, double duration)
{
    // Node order matches the flow names, followed by the gateway and GCS.
    NodeContainer nodes;
    nodes.Create(5);
    auto positions = CreateObject<ListPositionAllocator>();
    positions->Add(Vector(500 + distance, 500, -20));
    positions->Add(Vector(500 - distance, 500, -20));
    positions->Add(Vector(500, 550, 40));
    positions->Add(Vector(500, 500, 0));
    positions->Add(Vector(575, 500, 0));
    MobilityHelper mobility;
    mobility.SetPositionAllocator(positions);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(nodes);

    NodeContainer acoustic(nodes.Get(0), nodes.Get(1), nodes.Get(3));
    auto channel = CreateObject<UanChannel>();
    channel->SetPropagationModel(CreateObject<UanPropModelThorp>());
    channel->SetNoiseModel(CreateObject<UanNoiseModelDefault>());
    UanModesList modes;
    modes.AppendMode(UanTxModeFactory::CreateMode(UanTxMode::FSK, 4800, 4800, 24000,
                                                 4800, 2, "SurfStarterFsk"));
    UanHelper uan;
    uan.SetPhy("ns3::UanPhyGen", "SupportedModes", UanModesListValue(modes));
    const auto uanDevices = uan.Install(acoustic, channel);

    PacketSocketHelper packetSockets;
    packetSockets.Install(acoustic);
    const auto raw = PacketSocketFactory::GetTypeId();
    for (std::size_t i = 0; i < 2; ++i)
    {
        uanTx[i] = Socket::CreateSocket(nodes.Get(i), raw);
        NS_ABORT_MSG_IF(uanTx[i]->Bind() != 0, "Cannot bind UUV socket");
        acousticDestination[i].SetSingleDevice(uanDevices.Get(i)->GetIfIndex());
        acousticDestination[i].SetProtocol(0);
        acousticDestination[i].SetPhysicalAddress(uanDevices.Get(2)->GetAddress());
    }

    PacketSocketAddress listen;
    listen.SetSingleDevice(uanDevices.Get(2)->GetIfIndex());
    listen.SetProtocol(0);
    uanRx = Socket::CreateSocket(nodes.Get(3), raw);
    NS_ABORT_MSG_IF(uanRx->Bind(listen) != 0, "Cannot bind gateway UAN socket");
    uanRx->SetRecvCallback(MakeCallback(&CoastalScenario::ReceiveAtGateway, this));

    YansWifiChannelHelper rfChannel;
    rfChannel.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
    rfChannel.AddPropagationLoss("ns3::FriisPropagationLossModel",
                                 "Frequency", DoubleValue(5.18e9));
    YansWifiPhyHelper phy;
    phy.SetChannel(rfChannel.Create());
    phy.Set("ChannelSettings", StringValue("{36, 20, BAND_5GHZ, 0}"));
    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211ac);
    wifi.SetRemoteStationManager("ns3::IdealWifiManager");
    WifiMacHelper mac;
    const Ssid ssid("surf-starter");
    mac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(ssid), "ActiveProbing", BooleanValue(false));
    auto wifiDevices = wifi.Install(phy, mac, NodeContainer(nodes.Get(2), nodes.Get(4)));
    mac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(ssid));
    wifiDevices.Add(wifi.Install(phy, mac, nodes.Get(3)));

    InternetStackHelper internet;
    internet.Install(NodeContainer(nodes.Get(2), nodes.Get(4), nodes.Get(3)));
    Ipv4AddressHelper ipv4;
    ipv4.SetBase("10.50.0.0", "255.255.255.0");
    const auto addresses = ipv4.Assign(wifiDevices);
    gcsAddress = addresses.GetAddress(1);
    gatewayAddress = addresses.GetAddress(2);

    const auto udp = UdpSocketFactory::GetTypeId();
    uavTx = Socket::CreateSocket(nodes.Get(2), udp);
    relayTx = Socket::CreateSocket(nodes.Get(3), udp);
    rfRx = Socket::CreateSocket(nodes.Get(3), udp);
    NS_ABORT_MSG_IF(rfRx->Bind(InetSocketAddress(gatewayAddress, GatewayPort)) != 0,
                    "Cannot bind gateway UDP socket");
    rfRx->SetRecvCallback(MakeCallback(&CoastalScenario::ReceiveAtGateway, this));
    gcsRx = Socket::CreateSocket(nodes.Get(4), udp);
    NS_ABORT_MSG_IF(gcsRx->Bind(InetSocketAddress(gcsAddress, GcsPort)) != 0,
                    "Cannot bind GCS UDP socket");
    gcsRx->SetRecvCallback(MakeCallback(&CoastalScenario::ReceiveAtGcs, this));

    // Allow time for Wi-Fi association and for in-flight packets at the end.
    for (uint32_t tick = 0; 2 + tick / (3 * rate) < duration - 2; ++tick)
    {
        const auto id = static_cast<uint32_t>(records.size());
        records.push_back({tick % Sources.size()});
        Simulator::Schedule(Seconds(2 + tick / (3 * rate)), &CoastalScenario::Send, this, id);
    }
    if (outage)
    {
        Simulator::Schedule(Seconds(OutageEnd), &CoastalScenario::Flush, this);
    }
}

void
CoastalScenario::Send(uint32_t id)
{
    auto& row = records.at(id);
    row.tx = Simulator::Now().GetNanoSeconds();
    std::array<uint8_t, PayloadBytes> payload{};
    payload[0] = static_cast<uint8_t>(id >> 24);
    payload[1] = static_cast<uint8_t>(id >> 16);
    payload[2] = static_cast<uint8_t>(id >> 8);
    payload[3] = static_cast<uint8_t>(id);
    const auto packet = Create<Packet>(payload.data(), payload.size());

    if (row.source < 2)
    {
        if (uanTx[row.source]->SendTo(packet, 0, acousticDestination[row.source]) < 0)
        {
            row.outcome = "source_rejected";
        }
    }
    else if (uavTx->SendTo(packet, 0, InetSocketAddress(gatewayAddress, GatewayPort)) < 0)
    {
        row.outcome = "source_rejected";
    }
}

void
CoastalScenario::ReceiveAtGateway(Ptr<Socket> socket)
{
    while (auto packet = socket->Recv())
    {
        const auto id = ReadId(packet);
        auto& row = records.at(id);
        if (row.gatewayRx >= 0)
        {
            continue;
        }

        row.gatewayRx = Simulator::Now().GetNanoSeconds();
        const double now = Simulator::Now().GetSeconds();
        if (outage && now >= OutageStart && now < OutageEnd)
        {
            if (pending.size() < bufferPackets)
            {
                pending.emplace_back(packet, id);
            }
            else
            {
                row.outcome = "gateway_dropped";
            }
            continue;
        }
        Forward(packet, id);
    }
}

void
CoastalScenario::Forward(Ptr<Packet> packet, uint32_t id)
{
    auto& row = records.at(id);
    row.forward = Simulator::Now().GetNanoSeconds();
    row.outcome = relayTx->SendTo(packet, 0, InetSocketAddress(gcsAddress, GcsPort)) < 0
                      ? "relay_rejected"
                      : "no_gcs_rx";
}

void
CoastalScenario::Flush()
{
    // This simple FIFO releases buffered packets to the RF stack at recovery.
    for (const auto& [packet, id] : pending)
    {
        Forward(packet, id);
    }
    pending.clear();
}

void
CoastalScenario::ReceiveAtGcs(Ptr<Socket> socket)
{
    while (auto packet = socket->Recv())
    {
        auto& row = records.at(ReadId(packet));
        if (row.gcsRx < 0)
        {
            row.gcsRx = Simulator::Now().GetNanoSeconds();
            row.outcome = "delivered";
        }
    }
}

void
CoastalScenario::WriteResults(double duration, const std::string& prefix) const
{
    std::ofstream packets(prefix + "-packets.csv");
    std::ofstream summary(prefix + "-summary.csv");
    NS_ABORT_MSG_UNLESS(packets && summary, "Cannot open output files for " << prefix);

    packets << "packet_id,source,tx_ns,gateway_rx_ns,forward_ns,gcs_rx_ns,outcome\n";
    for (std::size_t id = 0; id < records.size(); ++id)
    {
        const auto& row = records[id];
        packets << id << ',' << Sources[row.source] << ',' << row.tx << ',' << row.gatewayRx
                << ',' << row.forward << ',' << row.gcsRx << ',' << row.outcome << '\n';
    }

    summary << "source,generated,gateway_rx,gcs_rx,pdr_pct,goodput_bps,mean_e2e_ms,p95_e2e_ms\n";
    summary << std::fixed << std::setprecision(4);
    for (std::size_t i = 0; i < Sources.size(); ++i)
    {
        uint32_t generated = 0;
        uint32_t receivedAtGateway = 0;
        double sumMs = 0;
        std::vector<double> delays;
        for (const auto& row : records)
        {
            if (row.source != i)
            {
                continue;
            }
            ++generated;
            receivedAtGateway += row.gatewayRx >= 0;
            if (row.gcsRx >= 0)
            {
                const double ms = (row.gcsRx - row.tx) / 1e6;
                sumMs += ms;
                delays.push_back(ms);
            }
        }

        const auto received = delays.size();
        summary << Sources[i] << ',' << generated << ',' << receivedAtGateway << ',' << received
                << ',' << 100.0 * received / generated
                << ',' << 8.0 * PayloadBytes * received / duration << ',';
        if (!delays.empty())
        {
            std::sort(delays.begin(), delays.end());
            const auto p95 = static_cast<std::size_t>(std::ceil(0.95 * received)) - 1;
            summary << sumMs / received << ',' << delays[p95];
        }
        else
        {
            summary << ',';
        }
        summary << '\n';
    }
    packets.flush();
    summary.flush();
    NS_ABORT_MSG_UNLESS(packets && summary, "Cannot write output files for " << prefix);
}

} // namespace

int
main(int argc, char* argv[])
{
    double distance = 150;
    double rate = 1;
    double duration = 30;
    std::string prefix = "surf";
    CoastalScenario scenario;

    CommandLine cmd(__FILE__);
    cmd.AddValue("distance", "Horizontal UUV-to-gateway distance in metres (1 to 500)", distance);
    cmd.AddValue("rate", "Packets generated per second per source (0.2 to 20)", rate);
    cmd.AddValue("duration", "Simulation duration in seconds (10 to 600)", duration);
    cmd.AddValue("outage", "Pause gateway-to-GCS forwarding during [10 s, 15 s)", scenario.outage);
    cmd.AddValue("buffer", "Maximum packets buffered by the gateway during the outage",
                 scenario.bufferPackets);
    cmd.AddValue("prefix", "Output path prefix (existing parent directory)", prefix);
    cmd.Parse(argc, argv);

    NS_ABORT_MSG_UNLESS(distance >= 1 && distance <= 500 && rate >= 0.2 && rate <= 20 &&
                            duration >= (scenario.outage ? 17 : 10) && duration <= 600,
                        "Invalid range, packet rate, or duration");
    scenario.Configure(distance, rate, duration);
    Simulator::Stop(Seconds(duration));
    Simulator::Run();
    scenario.WriteResults(duration, prefix);
    Simulator::Destroy();
    std::cout << "Wrote " << prefix << "-packets.csv and " << prefix << "-summary.csv\n";
    return 0;
}
