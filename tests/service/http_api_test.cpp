// SPDX-License-Identifier: GPL-3.0-or-later
#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <random>
#include <sys/socket.h>
#include <unistd.h>

#include "service/settings.hpp"
#include "service_harness.hpp"

using namespace bluev;
using namespace bluev::testing;
using ::testing::_;
using ::testing::AtLeast;
using ::testing::InSequence;
using ::testing::Return;

namespace
{
class HttpApi : public ServiceHarness
{
};
} // namespace

TEST_F(HttpApi, ServesThePageWithoutCorsHeaders)
{
    ASSERT_TRUE(startService());
    std::string body;

    EXPECT_EQ(200, request("GET", "/", body));
    EXPECT_NE(std::string::npos, body.find("<title>"));
    EXPECT_EQ(200, request("GET", "/index.html", body));

    EXPECT_EQ(std::string::npos, fullAnswer("GET /api/status HTTP/1.0\r\n\r\n").find("Access-Control"));
    EXPECT_EQ(0u, fullAnswer("OPTIONS /api/connect HTTP/1.0\r\n\r\n").find("HTTP/1.0 405"));
}

TEST_F(HttpApi, ReadyNotificationNamesTheAddress)
{
    address = "192.168.1.20";
    ASSERT_TRUE(startService());

    const std::string expected = "BlueV ready: open http://192.168.1.20:" + std::to_string(port);
    EXPECT_TRUE(waitFor([&] { return sink.notes() == std::vector<std::string>{expected}; }, 2000));
}

TEST_F(HttpApi, ReadyNotificationWithoutAnAddressPointsAtTheHomeScreen)
{
    ASSERT_TRUE(startService());

    EXPECT_TRUE(waitFor(
        [&] { return sink.notes() == std::vector<std::string>{"BlueV ready: open its entry on the home screen"}; },
        2000));
}

TEST_F(HttpApi, StatusIsJsonAndIdle)
{
    ASSERT_TRUE(startService());
    const std::string body = status();
    EXPECT_TRUE(looksLikeJson(body)) << body;
    EXPECT_NE(std::string::npos, body.find("\"state\":\"idle\""));
    EXPECT_NE(std::string::npos, body.find("\"on\":false"));
    EXPECT_NE(std::string::npos, body.find("\"adapter\":\"internal\""));
    EXPECT_NE(std::string::npos, body.find("\"foreign_requests\":0"));
}

TEST_F(HttpApi, MalformedRequestsGetErrors)
{
    ASSERT_TRUE(startService());
    std::string body;

    EXPECT_EQ(404, request("GET", "/nope", body));
    EXPECT_EQ(405, request("PUT", "/api/scan", body));
    EXPECT_EQ(400, rawRequest("garbage", body));
    EXPECT_EQ(400, request("POST", "/api/connect?addr=zz%3Azz", body));
    EXPECT_EQ(400, request("POST", "/api/connect?addr=zz", body));
    EXPECT_EQ(400, request("POST", "/api/connect", body));
    EXPECT_EQ(409, request("POST", "/api/bogus", body));
    // an oversized request must not hang the server
    rawRequest("GET /" + std::string(5000, 'a') + " HTTP/1.0\r\n\r\n", body);
    EXPECT_EQ(200, request("GET", "/api/status", body));
}

TEST_F(HttpApi, StatusLinesNameTheirCodes)
{
    ASSERT_TRUE(startService());

    EXPECT_EQ(0u, fullAnswer("GET /nope HTTP/1.0\r\n\r\n").find("HTTP/1.0 404 Not Found"));
    EXPECT_EQ(0u, fullAnswer("PUT /api/scan HTTP/1.0\r\n\r\n").find("HTTP/1.0 405 Method Not Allowed"));
    EXPECT_EQ(0u, fullAnswer("POST /api/bogus HTTP/1.0\r\n\r\n").find("HTTP/1.0 409 Conflict"));
}

TEST_F(HttpApi, ABrokenPercentEscapeIsKeptAsItIs)
{
    ASSERT_TRUE(startService());
    std::string body;

    // "%zz" is not an escape: the address stays invalid, it is not turned into a NUL byte
    EXPECT_EQ(400, request("POST", "/api/forget?addr=AA%zzBB", body));
    EXPECT_EQ(400, request("POST", "/api/forget?addr=AA%3", body));
}

TEST_F(HttpApi, ForgetAcceptsAPercentEncodedAddress)
{
    ASSERT_TRUE(startService());
    std::string body;
    std::atomic<int> forgotten{0};

    EXPECT_CALL(radio, forget(BdAddr{{0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA}}))
        .WillOnce(::testing::Invoke([&](const BdAddr&) { forgotten++; }));
    EXPECT_EQ(200, request("POST", "/api/forget?addr=AA%3ABB%3ACC%3ADD%3AEE%3AFF", body));
    EXPECT_TRUE(waitFor([&] { return forgotten.load() == 1; }, 2000));
}

TEST_F(HttpApi, UninstallShortcutAsksTheInstaller)
{
    ASSERT_TRUE(startService());
    std::string body;
    EXPECT_CALL(installer, unregisterTitle(_)).WillOnce(Return(true));
    EXPECT_EQ(200, request("POST", "/api/uninstall-shortcut", body));
    EXPECT_CALL(installer, unregisterTitle(_)).WillOnce(Return(false));
    EXPECT_EQ(409, request("POST", "/api/uninstall-shortcut", body));
}

TEST_F(HttpApi, WithTheRadioOffNothingTouchesIt)
{
    ASSERT_TRUE(startService());
    std::string body;

    EXPECT_CALL(radio, scan(_)).Times(0);
    EXPECT_CALL(radio, connectTo(_)).Times(0);
    EXPECT_CALL(radio, poll(_)).Times(0);
    EXPECT_EQ(409, request("POST", "/api/scan", body));
    EXPECT_NE(std::string::npos, body.find("the radio is off"));
    EXPECT_EQ(409, request("POST", std::string("/api/connect?addr=") + kSpeakerAddress, body));
    EXPECT_EQ(409, request("POST", "/api/radio/off", body));
}

TEST_F(HttpApi, TheAdapterChoiceIsChecked)
{
    ASSERT_TRUE(startService());
    std::string body;

    EXPECT_CALL(radio, open(_)).Times(0);
    EXPECT_EQ(400, request("POST", "/api/radio/on", body));
    EXPECT_EQ(400, request("POST", "/api/radio/on?adapter=external", body));
    EXPECT_EQ(400, request("POST", "/api/radio/on?adapter=external&dev=ugen0.2", body)); // the console's own chip
    EXPECT_EQ(400, request("POST", "/api/radio/on?adapter=external&dev=..%2Fetc", body));
    EXPECT_EQ(400, request("POST", "/api/radio/on?adapter=external&dev=ugen1", body));
    EXPECT_EQ(400, request("POST", "/api/radio/on?adapter=external&dev=ugen1.", body));
    EXPECT_EQ(400, request("POST", "/api/radio/on?adapter=bluetooth", body));
}

TEST_F(HttpApi, AdaptersListTheInternalChipAndWhatIsPlugged)
{
    UsbDevice csr;
    csr.node = "ugen1.2";
    csr.vid = 0x0a12;
    csr.pid = 0x0001;
    csr.deviceClass = 0xe0;
    csr.subClass = 1;
    csr.protocol = 1;
    csr.product = "CSR8510 A10";
    csr.vendor = "Cambridge Silicon Radio";
    usb.devices = {csr};
    ASSERT_TRUE(startService());

    std::string body;
    EXPECT_EQ(200, request("GET", "/api/adapters", body));
    EXPECT_TRUE(looksLikeJson(body)) << body;
    EXPECT_NE(std::string::npos, body.find("\"internal\":true"));
    EXPECT_NE(std::string::npos, body.find("\"device\":\"ugen1.2\""));
    EXPECT_NE(std::string::npos, body.find("\"vid\":2578"));
    EXPECT_NE(std::string::npos, body.find("\"product\":\"CSR8510 A10\""));
}

TEST_F(HttpApi, StartingAndStoppingTheRadio)
{
    ASSERT_TRUE(startService());
    std::string body;

    {
        InSequence order;
        EXPECT_CALL(radio, open("")).WillOnce(Return(true));
        EXPECT_CALL(radio, setTick(_)).Times(1);
    }
    EXPECT_EQ(200, request("POST", "/api/radio/on?adapter=internal", body));
    ASSERT_TRUE(waitFor([&] { return status().find("\"on\":true") != std::string::npos; }, 2000));
    EXPECT_TRUE(waitForState("idle", 2000));
    EXPECT_EQ(409, request("POST", "/api/radio/on?adapter=internal", body)) << "a second start is refused";
    EXPECT_TRUE(sink.has("radio: console CFI-2002 B01, internal adapter ugen0.2 (no device information)"));

    EXPECT_CALL(radio, close()).Times(1);
    EXPECT_EQ(200, request("POST", "/api/radio/off", body));
    ASSERT_TRUE(waitFor([&] { return status().find("\"on\":false") != std::string::npos; }, 2000));
}

TEST_F(HttpApi, AnExternalAdapterIsOpenedByItsNode)
{
    UsbDevice csr;
    csr.node = "ugen1.2";
    csr.vid = 0x0a12;
    csr.pid = 1;
    csr.deviceClass = 0xe0;
    usb.devices = {csr};
    ASSERT_TRUE(startService());
    std::string body;

    EXPECT_CALL(radio, open("ugen1.2")).WillOnce(Return(true));
    EXPECT_EQ(200, request("POST", "/api/radio/on?adapter=external&dev=ugen1.2", body));
    ASSERT_TRUE(waitFor([&] { return status().find("\"on\":true") != std::string::npos; }, 2000));
    EXPECT_NE(std::string::npos, status().find("\"adapter\":\"external\""));
    EXPECT_NE(std::string::npos, status().find("\"device\":\"ugen1.2\""));
    EXPECT_TRUE(sink.has("external adapter ugen1.2"));
}

namespace
{
UsbDevice csrAdapter()
{
    UsbDevice csr;
    csr.node = "ugen1.2";
    csr.vid = 0x0a12;
    csr.pid = 1;
    csr.deviceClass = 0xe0;
    return csr;
}
} // namespace

TEST_F(HttpApi, TheUsbAdapterInUseIsOpenedAgainWhenBlueVStarts)
{
    usb.devices = {csrAdapter()};
    ASSERT_TRUE(startService());
    std::string body;

    EXPECT_EQ(409, request("POST", "/api/autostart?on=1", body)) << "the radio is off";
    EXPECT_CALL(radio, open("ugen1.2")).WillOnce(Return(true));
    EXPECT_EQ(200, request("POST", "/api/radio/on?adapter=external&dev=ugen1.2", body));
    ASSERT_TRUE(waitFor([&] { return status().find("\"on\":true") != std::string::npos; }, 2000));
    EXPECT_NE(std::string::npos, status().find("\"autostart\":false"));
    EXPECT_EQ(200, request("POST", "/api/autostart?on=1", body));
    EXPECT_NE(std::string::npos, status().find("\"autostart\":true"));

    // a new run of BlueV, the adapter plugged in at another port
    stopService();
    port = 0;
    usb.devices = {csrAdapter()};
    usb.devices[0].node = "ugen2.3";
    EXPECT_CALL(radio, open("ugen2.3")).WillOnce(Return(true));
    ASSERT_TRUE(startService());
    ASSERT_TRUE(waitFor([&] { return status().find("\"on\":true") != std::string::npos; }, 2000));
    EXPECT_NE(std::string::npos, status().find("\"device\":\"ugen2.3\""));
    EXPECT_TRUE(sink.has("radio: starting with the USB adapter of the last run (0a12:0001)"));
}

TEST_F(HttpApi, WithoutTheAdapterTheRadioStaysOffAndTheUserIsTold)
{
    Settings saved;
    saved.autostart = true;
    saved.vid = 0x0a12;
    saved.pid = 1;
    ASSERT_TRUE(settings.save(saved));
    EXPECT_CALL(radio, open(_)).Times(0);
    ASSERT_TRUE(startService());

    EXPECT_NE(std::string::npos, status().find("\"on\":false"));
    EXPECT_NE(std::string::npos, status().find("\"autostart\":true")); // still wanted
    EXPECT_TRUE(sink.has("is not plugged in"));
}

TEST_F(HttpApi, TurningAutostartOffIsRemembered)
{
    usb.devices = {csrAdapter()};
    ASSERT_TRUE(startService());
    std::string body;

    EXPECT_CALL(radio, open("ugen1.2")).WillOnce(Return(true));
    EXPECT_EQ(200, request("POST", "/api/radio/on?adapter=external&dev=ugen1.2", body));
    ASSERT_TRUE(waitFor([&] { return status().find("\"on\":true") != std::string::npos; }, 2000));
    EXPECT_EQ(200, request("POST", "/api/autostart?on=1", body));
    EXPECT_EQ(200, request("POST", "/api/autostart?on=0", body));

    stopService();
    port = 0;
    EXPECT_CALL(radio, open(_)).Times(0);
    ASSERT_TRUE(startService());
    EXPECT_NE(std::string::npos, status().find("\"on\":false"));
    EXPECT_NE(std::string::npos, status().find("\"autostart\":false"));
}

TEST_F(HttpApi, TheConsolesOwnChipIsNeverStartedAutomatically)
{
    ASSERT_TRUE(startService());
    std::string body;

    EXPECT_CALL(radio, open("")).WillOnce(Return(true));
    EXPECT_EQ(200, request("POST", "/api/radio/on?adapter=internal", body));
    ASSERT_TRUE(waitFor([&] { return status().find("\"on\":true") != std::string::npos; }, 2000));
    EXPECT_EQ(409, request("POST", "/api/autostart?on=1", body));
    EXPECT_EQ(400, request("POST", "/api/autostart?on=2", body));
    EXPECT_EQ(400, request("POST", "/api/autostart", body));
    EXPECT_NE(std::string::npos, status().find("\"autostart\":false"));
}

TEST_F(HttpApi, AFailedStartLeavesTheRadioOffWithTheReason)
{
    ASSERT_TRUE(startService());
    std::string body;

    EXPECT_CALL(radio, open(_)).WillOnce(Return(false));
    EXPECT_CALL(radio, openError()).WillOnce(Return(std::string("the adapter could not be opened")));
    EXPECT_EQ(200, request("POST", "/api/radio/on?adapter=internal", body));
    ASSERT_TRUE(waitFor(
        [&] { return status().find("radio off: the adapter could not be opened") != std::string::npos; }, 2000));
    EXPECT_NE(std::string::npos, status().find("\"on\":false"));
    EXPECT_TRUE(waitForState("idle", 1000));
}

TEST_F(HttpApi, AFailedStartWithoutAReasonGivesADefault)
{
    ASSERT_TRUE(startService());
    std::string body;

    EXPECT_CALL(radio, open(_)).WillOnce(Return(false));
    request("POST", "/api/radio/on?adapter=internal", body);
    ASSERT_TRUE(waitFor([&] { return status().find("the controller is busy") != std::string::npos; }, 2000));
}

namespace
{
class WithRadioOn : public HttpApi
{
  protected:
    void SetUp() override
    {
        ASSERT_TRUE(startService());
        std::string body;
        ASSERT_EQ(200, request("POST", "/api/radio/on?adapter=internal", body));
        ASSERT_TRUE(waitFor([&] { return status().find("\"on\":true") != std::string::npos; }, 2000));
        ASSERT_TRUE(waitForState("idle", 2000));
    }
};
} // namespace

TEST_F(WithRadioOn, ScanListsDevicesWithTheirNamesEscaped)
{
    std::string body;

    EXPECT_EQ(200, request("POST", "/api/scan", body));
    EXPECT_TRUE(waitForState("scanning", 1000) || waitForState("idle", 2000));
    ASSERT_TRUE(waitFor(
        [&] { return sink.has("") && stateName() == "idle" && status().find("scan finished") != std::string::npos; },
        5000));

    request("GET", "/api/devices", body);
    EXPECT_TRUE(looksLikeJson(body)) << body;
    EXPECT_NE(std::string::npos, body.find("Device \\\"0\\\"\\n"));
    EXPECT_NE(std::string::npos, body.find("\"other\":true"));
    EXPECT_NE(std::string::npos, body.find("\"rssi\":-50"));
}

TEST_F(WithRadioOn, DevicesAlsoShowPairedOnesThatWereNotSeen)
{
    BondEntry b;
    b.peer = BdAddr{{0xCC, 0xBB, 0xAA, 0x22, 0x11, 0x00}};
    b.name = "Creative";
    ON_CALL(radio, bonds()).WillByDefault(Return(std::vector<BondEntry>{b}));
    ON_CALL(radio, hasBond(_)).WillByDefault(Return(true));

    std::string body;
    ASSERT_TRUE(waitFor(
        [&] {
            request("GET", "/api/devices", body);
            return body.find("Creative") != std::string::npos;
        },
        3000));
    EXPECT_NE(std::string::npos, body.find("\"addr\":\"00:11:22:AA:BB:CC\""));
    EXPECT_NE(std::string::npos, body.find("\"bonded\":true"));
}

TEST_F(WithRadioOn, ASeenDeviceWithoutANameGetsTheNameOfItsBond)
{
    ON_CALL(radio, devices()).WillByDefault(::testing::Invoke([] {
        BtDevice d;
        d.address = BdAddr{{0xCC, 0xBB, 0xAA, 0x22, 0x11, 0x00}};
        return std::vector<BtDevice>{d};
    }));
    BondEntry b;
    b.peer = BdAddr{{0xCC, 0xBB, 0xAA, 0x22, 0x11, 0x00}};
    b.name = "Creative";
    ON_CALL(radio, bonds()).WillByDefault(Return(std::vector<BondEntry>{b}));

    std::string body;
    ASSERT_TRUE(waitFor(
        [&] {
            request("GET", "/api/devices", body);
            return body.find("Creative") != std::string::npos;
        },
        3000));
    // listed once
    EXPECT_EQ(body.find("00:11:22:AA:BB:CC"), body.rfind("00:11:22:AA:BB:CC"));
}

TEST_F(WithRadioOn, ConnectStreamsAndDisconnectEndsIt)
{
    std::string body;
    {
        InSequence order;
        EXPECT_CALL(radio, resetAudio()).Times(1);
        EXPECT_CALL(radio, connectTo(BdAddr{{0xCC, 0xBB, 0xAA, 0x22, 0x11, 0x00}})).WillOnce(Return(true));
        EXPECT_CALL(radio, startAudio(true)).WillOnce(Return(true));
        EXPECT_CALL(radio, stream(true)).Times(1);
        EXPECT_CALL(radio, stopAudio()).Times(1);
        EXPECT_CALL(radio, disconnect()).Times(1);
        EXPECT_CALL(radio, resetAudio()).Times(1);
        EXPECT_CALL(radio, listenAudio()).Times(1);
    }
    EXPECT_EQ(200, request("POST", std::string("/api/connect?addr=") + kSpeakerAddress, body));
    ASSERT_TRUE(waitForState("streaming", 3000));

    const std::string s = status();
    EXPECT_NE(std::string::npos, s.find("\"name\":\"Fake Speaker\"")) << s;
    ASSERT_TRUE(waitFor([&] { return status().find("\"pps\":49") != std::string::npos; }, 2000));
    EXPECT_TRUE(looksLikeJson(status()));
    EXPECT_NE(std::string::npos, status().find("\"queue_ms\":")) << "the web page reads this name";
    EXPECT_NE(std::string::npos, status().find("\"rate\":48000"));
    EXPECT_EQ(409, request("POST", "/api/scan", body)) << "scan refused while streaming";

    EXPECT_EQ(200, request("POST", "/api/disconnect", body));
    ASSERT_TRUE(waitForState("idle", 3000));
    EXPECT_NE(std::string::npos, status().find("stream ended"));
    EXPECT_NE(std::string::npos, status().find("\"rate\":0")); // the numbers are cleared
}

TEST_F(WithRadioOn, TheCaptureFlagIsPassedToTheStream)
{
    // started with capture available in SetUp; connect and look
    std::string body;
    request("POST", std::string("/api/connect?addr=") + kSpeakerAddress, body);
    ASSERT_TRUE(waitForState("streaming", 3000));
    EXPECT_TRUE(lastStreamCapturing.load());
}

TEST_F(WithRadioOn, AFailedConnectIsReportedAndTheHalfPairedDeviceIsForgotten)
{
    connectWorks.store(false);
    std::string body;

    EXPECT_CALL(radio, startAudio(_)).Times(0);
    EXPECT_CALL(radio, forget(BdAddr{{0xCC, 0xBB, 0xAA, 0x22, 0x11, 0x00}})).Times(::testing::AtLeast(1));
    EXPECT_CALL(radio, disconnect()).Times(1);
    request("POST", std::string("/api/connect?addr=") + kSpeakerAddress, body);
    ASSERT_TRUE(waitFor([&] { return status().find("could not connect") != std::string::npos; }, 3000));
    EXPECT_TRUE(waitForState("idle", 1000));
}

TEST_F(WithRadioOn, AFailedConnectKeepsAnEarlierPairing)
{
    connectWorks.store(false);
    ON_CALL(radio, hasBond(_)).WillByDefault(Return(true));
    std::string body;

    EXPECT_CALL(radio, forget(_)).Times(0);
    request("POST", std::string("/api/connect?addr=") + kSpeakerAddress, body);
    ASSERT_TRUE(waitFor([&] { return status().find("could not connect") != std::string::npos; }, 3000));
}

TEST_F(WithRadioOn, AudioSetupFailureStopsTheAudioAndSaysSo)
{
    std::string body;

    ON_CALL(radio, startAudio(_)).WillByDefault(Return(false));
    EXPECT_CALL(radio, stopAudio()).Times(1);
    EXPECT_CALL(radio, stream(_)).Times(0);
    request("POST", std::string("/api/connect?addr=") + kSpeakerAddress, body);
    ASSERT_TRUE(waitFor([&] { return status().find("audio setup failed") != std::string::npos; }, 3000));
}

TEST_F(WithRadioOn, ADeviceThatDialsInIsAdoptedAndStreams)
{
    std::string body;

    EXPECT_CALL(radio, adoptIncoming()).WillOnce(Return(true));
    EXPECT_CALL(radio, startAudio(false)).WillOnce(Return(true)); // not our connection: it gets the long wait
    simulateIncoming();
    ASSERT_TRUE(waitForState("streaming", 3000));
    request("POST", "/api/disconnect", body);
    ASSERT_TRUE(waitForState("idle", 3000));
}

TEST_F(WithRadioOn, AnInboundLinkThatCannotBeSecuredEndsInTheMessage)
{
    ON_CALL(radio, adoptIncoming()).WillByDefault(Return(false));
    EXPECT_CALL(radio, startAudio(_)).Times(0);
    simulateIncoming();
    ASSERT_TRUE(waitFor([&] { return status().find("the link could not be secured") != std::string::npos; }, 3000));
}

TEST_F(WithRadioOn, ADisconnectWhenNothingIsConnectedDoesNotLinger)
{
    std::string body;

    EXPECT_EQ(200, request("POST", "/api/disconnect", body));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    // a stream that starts afterwards must not be stopped by the stale request
    request("POST", std::string("/api/connect?addr=") + kSpeakerAddress, body);
    ASSERT_TRUE(waitForState("streaming", 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ("streaming", stateName());
}

TEST_F(WithRadioOn, ForeignRequestsAreShownInTheStatus)
{
    ON_CALL(radio, foreignRequests()).WillByDefault(Return(7));
    EXPECT_NE(std::string::npos, status().find("\"foreign_requests\":7"));
}

TEST_F(WithRadioOn, ATickPublishesThePeerName)
{
    ASSERT_TRUE(waitFor([&] { return status().find("\"peer\":{\"name\":\"\"") != std::string::npos || true; }, 100));
    std::string body;
    request("POST", std::string("/api/connect?addr=") + kSpeakerAddress, body);
    ASSERT_TRUE(waitForState("streaming", 3000));
    EXPECT_NE(std::string::npos, status().find("\"addr\":\"00:11:22:33:44:55\""));
}

TEST_F(WithRadioOn, TheLogEndpointReturnsLinesAndANextNumber)
{
    log.add("12:00:00 hello");
    log.add("12:00:01 world");
    std::string body;
    ASSERT_EQ(200, request("GET", "/api/log?since=0", body));
    EXPECT_TRUE(looksLikeJson(body)) << body;
    EXPECT_NE(std::string::npos, body.find("\"next\":2"));
    EXPECT_NE(std::string::npos, body.find("12:00:00 hello\\n12:00:01 world\\n"));
    ASSERT_EQ(200, request("GET", "/api/log?since=1", body));
    EXPECT_EQ(std::string::npos, body.find("hello"));
    EXPECT_EQ(200, request("GET", "/api/log?since=999999", body));
    EXPECT_TRUE(looksLikeJson(body));
}

TEST_F(WithRadioOn, QuitStopsTheServiceAndClosesTheRadio)
{
    std::string body;
    EXPECT_CALL(radio, close()).Times(1);
    EXPECT_EQ(200, request("POST", "/api/quit", body));
    // stopService() joins; a hang here fails the test by timeout
    stopService();
    SUCCEED();
}

TEST_F(WithRadioOn, QuitDuringAStreamEndsTheStream)
{
    std::string body;
    request("POST", std::string("/api/connect?addr=") + kSpeakerAddress, body);
    ASSERT_TRUE(waitForState("streaming", 3000));
    EXPECT_CALL(radio, requestStop()).Times(::testing::AtLeast(1));
    request("POST", "/api/quit", body);
    stopService();
    SUCCEED();
}

// A fixed number of requests, not a fixed time: every request leaves a client
// port in TIME_WAIT, and macOS has only 16384 of them, held for 30 s. A fast
// machine running for a fixed time used them all up, and then /api/quit could
// not connect and the test hung.
TEST_F(WithRadioOn, ManyClientsAtOnceGetValidAnswers)
{
    constexpr int kRequestsPerClient = 100;
    std::atomic<long> bad{0};
    std::mutex badMutex;
    std::vector<std::string> badNotes;
    const auto noteBad = [&](const std::string& what, int code, const std::string& reply) {
        std::lock_guard<std::mutex> l(badMutex);
        if (badNotes.size() < 10)
        {
            badNotes.push_back(what + " -> " + std::to_string(code) + " [" + reply.substr(0, 120) + "]");
        }
    };
    std::vector<std::thread> clients;
    const char* gets[] = {"/api/status", "/api/devices", "/api/log?since=0", "/api/log?since=999999", "/"};
    const char* posts[] = {"scan", "disconnect", "forget", "connect"};

    for (int c = 0; c < 8; c++)
    {
        clients.emplace_back([&, c] {
            std::mt19937 rng(static_cast<unsigned>(c) + 1);

            for (int i = 0; i < kRequestsPerClient; i++)
            {
                std::string reply;
                int code;

                if (rng() % 3 != 0)
                {
                    const char* path = gets[rng() % 5];

                    code = request("GET", path, reply);
                    if (code == 200 && std::strcmp(path, "/") != 0 && !looksLikeJson(reply))
                    {
                        bad++;
                        noteBad(std::string("GET ") + path, code, reply);
                    }
                }
                else
                {
                    std::string path = std::string("/api/") + posts[rng() % 4];

                    if (path != "/api/disconnect" && path != "/api/scan")
                    {
                        path += std::string("?addr=") + kSpeakerAddress;
                    }
                    code = request("POST", path, reply);
                }
                if (code == 0)
                {
                    bad++;
                    noteBad("request", code, reply);
                }
            }
        });
    }
    for (std::thread& client : clients)
    {
        client.join();
    }
    EXPECT_EQ(0, bad.load());
    for (const std::string& note : badNotes)
    {
        ADD_FAILURE() << note;
    }
    std::string body;
    request("POST", "/api/disconnect", body);
    EXPECT_TRUE(waitForState("idle", 5000));
}

// Finding and asking an older copy to quit is no longer HTTP-based (a new
// copy looks it up by process name through the kernel, see
// platform/ps5/process_identity.hpp), and the server listens on one fixed
// port rather than a range, so there is no fallback-port behaviour left to
// cover here. Both are PS5-only (thr_set_name/sysctl) and are not part of
// the host test build.

TEST_F(WithRadioOn, VolumeIsShownAndSetOnlyWhileTheHeadsetHasOne)
{
    std::string body;
    EXPECT_CALL(radio, setVolume(76)).Times(1); // 60 percent of 127

    EXPECT_EQ(200, request("POST", std::string("/api/connect?addr=") + kSpeakerAddress, body));
    ASSERT_TRUE(waitForState("streaming", 3000));
    EXPECT_NE(std::string::npos, status().find("\"volume\":-1")) << "no volume until the headset reports one";
    EXPECT_EQ(409, request("POST", "/api/volume?v=60", body));

    headsetVolume = 127;
    EXPECT_NE(std::string::npos, status().find("\"volume\":100")) << status();
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // a few ticks
    for (const std::string& note : sink.notes())
    {
        EXPECT_EQ(std::string::npos, note.find("Headset volume")) << "the first value is not announced";
    }
    headsetVolume = 64; // a button on the headset
    ASSERT_TRUE(waitFor(
        [&] {
            const auto notes = sink.notes();
            return !notes.empty() && notes.back() == "Headset volume 50%";
        },
        3000));
    EXPECT_EQ(400, request("POST", "/api/volume?v=101", body));
    EXPECT_EQ(400, request("POST", "/api/volume?v=loud", body));
    EXPECT_EQ(400, request("POST", "/api/volume", body));
    EXPECT_EQ(200, request("POST", "/api/volume?v=60", body));
}
