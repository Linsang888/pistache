#include <pistache/async.h>
#include <pistache/http.h>
#include <pistache/description.h>
#include <pistache/client.h>
#include <pistache/endpoint.h>

#include "gtest/gtest.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>


using namespace Pistache;

const int wait_time = 3;

struct TestSet
{
    size_t      bytes = 0;
    Http::Code  expectedCode = Http::Code::Ok;
    Http::Code  actualCode = Http::Code::Ok;
};

using PayloadTestSets = std::vector<TestSet>;

void testPayloads(Http::Client& client, const std::string& url, const PayloadTestSets& testPayloads) {
   // Client tests to make sure the payload is enforced
    std::mutex resultsetMutex;
    PayloadTestSets test_results;
    std::vector<Async::Promise<Http::Response>> responses;
    for (auto & t : testPayloads) {
        std::string payload(t.bytes, 'A');
        auto response = client.post(url).body(payload).timeout(std::chrono::seconds(wait_time)).send();
        response.then([t,&test_results,&resultsetMutex](Http::Response rsp) {
                TestSet res(t);
                res.actualCode = rsp.code();
                {
                    std::unique_lock<std::mutex> lock(resultsetMutex);
                    test_results.push_back(res);
                }
                }, Async::IgnoreException);
        responses.push_back(std::move(response));
    }

    auto sync = Async::whenAll(responses.begin(), responses.end());
    Async::Barrier<std::vector<Http::Response>> barrier(sync);
    barrier.wait_for(std::chrono::seconds(2*wait_time));

    for (auto & result : test_results) {
        ASSERT_EQ(result.expectedCode, result.actualCode);
    }
}

void handleEcho(const Rest::Request& /*request*/, Http::ResponseWriter response) {
    response.send(Http::Code::Ok, "", MIME(Text, Plain));
}

TEST(payload, from_description)
{
    const Address addr(Ipv4::any(), Port(0));
    const size_t threads = 20;
    const size_t maxPayload = 1024; // very small

    Rest::Description desc("Rest Description Test", "v1");
    Rest::Router router;

    desc
        .route(desc.post("/"))
        .bind(&handleEcho)
        .response(Http::Code::Ok, "Response to the /ready call");

    router.initFromDescription(desc);

    auto flags = Tcp::Options::ReuseAddr;
    auto opts = Http::Endpoint::options()
        .threads(threads)
        .flags(flags)
        .maxPayload(maxPayload);

    auto endpoint = std::make_shared<Pistache::Http::Endpoint>(addr);
    endpoint->init(opts);
    endpoint->setHandler(router.handler());
    endpoint->serveThreaded();


    Http::Client client;
    auto client_opts = Http::Client::options()
        .threads(3)
        .maxConnectionsPerHost(3);
    client.init(client_opts);

    // TODO: Remove temp hack once 'serveThreaded()' waits for socket to be
    // created before returning.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto port = endpoint->getPort();

    PayloadTestSets payloads{
       {800, Http::Code::Ok}
       , {1024, Http::Code::Request_Entity_Too_Large}
        ,{2048, Http::Code::Request_Entity_Too_Large}
    };

    testPayloads(client, "127.0.0.1:" + std::to_string(port), payloads);

    client.shutdown();
    endpoint->shutdown();
}

TEST(payload, manual_construction) {
    class MyHandler : public Http::Handler {
        public:
        HTTP_PROTOTYPE(MyHandler)

        void onRequest(
                const Http::Request& /*request*/,
                Http::ResponseWriter response) override
        {
            response.send(Http::Code::Ok, "All good");
        }
    private:
        tag placeholder;
    };

    // General test parameters.
    const Address addr(Ipv4::any(), Port(0));
    const int     threads = 20;
    const auto    flags = Tcp::Options::ReuseAddr;
    const size_t  maxPayload = 2048;

    // Build in-process server threads.
    auto endpoint = std::make_shared<Http::Endpoint>(addr);
    auto opts = Http::Endpoint::options()
        .threads(threads)
        .flags(flags)
        .maxPayload(maxPayload);

    endpoint->init(opts);
    endpoint->setHandler(Http::make_handler<MyHandler>());
    endpoint->serveThreaded();

    // TODO: Remove temp hack once 'serveThreaded()' waits for socket to be
    // created before returning.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto port = endpoint->getPort();

    // Create http client.
    Http::Client client;
    auto client_opts = Http::Client::options()
        .threads(3)
        .maxConnectionsPerHost(3);
    client.init(client_opts);

    PayloadTestSets payloads{
       {1024, Http::Code::Ok}
       , {1800, Http::Code::Ok}
       , {2048, Http::Code::Request_Entity_Too_Large}
       , {4096, Http::Code::Request_Entity_Too_Large}
    };
    testPayloads(client, "127.0.0.1:" + std::to_string(port), payloads);

    // Cleanup
    client.shutdown();
    endpoint->shutdown();
}
