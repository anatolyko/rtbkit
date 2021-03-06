/* http_exchange_connector.cc
   Jeremy Barnes, 31 January 2011
   Copyright (c) 2011 Datacratic.  All rights reserved.

   Auction endpoint class.
*/

#include "http_exchange_connector.h"
#include "http_auction_handler.h"
#include "jml/arch/exception.h"
#include "jml/arch/format.h"
#include "jml/arch/backtrace.h"
#include "jml/utils/guard.h"
#include "jml/utils/set_utils.h"
#include "jml/utils/vector_utils.h"
#include "jml/arch/timers.h"
#include "rtbkit/core/router/router.h"
#include <set>

#include <boost/foreach.hpp>


using namespace std;
using namespace ML;


namespace RTBKIT {


/*****************************************************************************/
/* HTTP EXCHANGE CONNECTOR                                                   */
/*****************************************************************************/

HttpExchangeConnector::
HttpExchangeConnector(const std::string & name,
                      ServiceBase & parent)
    : ExchangeConnector(name, parent),
      HttpEndpoint(name)
{
    postConstructorInit();
}

HttpExchangeConnector::
HttpExchangeConnector(const std::string & name,
                      std::shared_ptr<ServiceProxies> proxies)
    : ExchangeConnector(name, proxies),
      HttpEndpoint(name)
{
    postConstructorInit();
}

void
HttpExchangeConnector::
postConstructorInit()
{
    numThreads = 8;
    realTimePriority = -1;
    listenPort = 10001;
    bindHost = "*";
    performNameLookup = true;
    backlog = DEF_BACKLOG;
    pingTimeUnknownHostsMs = 20;

    numServingRequest_ = 0;
    acceptAuctionProbability = 1.0;

    // Link up events
    onTransportOpen = [=] (TransportBase *)
        {
            this->recordHit("auctionNewConnection");
        };

    onTransportClose = [=] (TransportBase *)
        {
            this->recordHit("auctionClosedConnection");
        };

    handlerFactory = [=] () { return new HttpAuctionHandler(); };
}

HttpExchangeConnector::
~HttpExchangeConnector()
{
    shutdown();
}

void
HttpExchangeConnector::
configure(const Json::Value & parameters)
{
    getParam(parameters, numThreads, "numThreads");
    getParam(parameters, realTimePriority, "realTimePriority");
    getParam(parameters, listenPort, "listenPort");
    getParam(parameters, bindHost, "bindHost");
    getParam(parameters, performNameLookup, "performNameLookup");
    getParam(parameters, backlog, "connectionBacklog");
    getParam(parameters, auctionResource, "auctionResource");
    getParam(parameters, auctionVerb, "auctionVerb");
    getParam(parameters, pingTimesByHostMs, "pingTimesByHostMs");
    getParam(parameters, pingTimeUnknownHostsMs, "pingTimeUnknownHostsMs");
}

void
HttpExchangeConnector::
configureHttp(int numThreads,
              const PortRange & listenPort,
              const std::string & bindHost,
              bool performNameLookup,
              int backlog,
              const std::string & auctionResource,
              const std::string & auctionVerb,
              int realTimePriority)
{
    this->numThreads = numThreads;
    this->realTimePriority = realTimePriority;
    this->listenPort = listenPort;
    this->bindHost = bindHost;
    this->performNameLookup = performNameLookup;
    this->backlog = backlog;
    this->auctionResource = auctionResource;
    this->auctionVerb = auctionVerb;
}

void
HttpExchangeConnector::
start()
{
    PassiveEndpoint::init(listenPort, bindHost, numThreads, true,
                          performNameLookup, backlog);
    if (realTimePriority > -1) {
        PassiveEndpoint::makeRealTime(realTimePriority);
    }
}

void
HttpExchangeConnector::
shutdown()
{
    HttpEndpoint::shutdown();
    ExchangeConnector::shutdown();
}

void
HttpExchangeConnector::
startRequestLogging(std::string const & filename, int count) {
    Guard guard(handlersLock);
    logger = std::make_shared<HttpAuctionLogger>(filename, count);
}

void
HttpExchangeConnector::
stopRequestLogging() {
    Guard guard(handlersLock);
    if(logger) {
        logger->close();
    }

    logger.reset();
}

std::shared_ptr<ConnectionHandler>
HttpExchangeConnector::
makeNewHandler()
{
    return makeNewHandlerShared();
}

std::shared_ptr<HttpAuctionHandler>
HttpExchangeConnector::
makeNewHandlerShared()
{
    if (!handlerFactory)
        throw ML::Exception("need to initialize handler factory");

    HttpAuctionHandler * handler = handlerFactory();
    if (!handler)
        throw ML::Exception("failed to create handler");

    std::shared_ptr<HttpAuctionHandler> handlerSp(handler);

    {
        Guard guard(handlersLock);
        if (logger) {
            handler->logger = logger;
        }

        handlers.insert(handlerSp);
    }

    return handlerSp;
}

void
HttpExchangeConnector::
finishedWithHandler(std::shared_ptr<HttpAuctionHandler> handler)
{
    Guard guard(handlersLock);
    handlers.erase(handler);
}

Json::Value
HttpExchangeConnector::
getServiceStatus() const
{
    Json::Value result;

    result["numConnections"] = numConnections();
    result["activeConnections"] = numServingRequest();
    result["connectionLoadFactor"]
        = xdiv<float>(numServingRequest(),
                      numConnections());
    
    map<string, int> peerCounts = numConnectionsByHost();
    
    BOOST_FOREACH(auto cnt, peerCounts)
        result["hostConnections"][cnt.first] = cnt.second;

    return result;
}

std::shared_ptr<BidRequest>
HttpExchangeConnector::
parseBidRequest(HttpAuctionHandler & connection,
                const HttpHeader & header,
                const std::string & payload)
{
    throw ML::Exception("need to override HttpExchangeConnector::parseBidRequest");
}

double
HttpExchangeConnector::
getTimeAvailableMs(HttpAuctionHandler & connection,
                   const HttpHeader & header,
                   const std::string & payload)
{
    throw ML::Exception("need to override HttpExchangeConnector::getTimeAvailableMs");
}

double
HttpExchangeConnector::
getRoundTripTimeMs(HttpAuctionHandler & connection,
                   const HttpHeader & header)
{
    string peerName = connection.transport().getPeerName();
    
    auto it = pingTimesByHostMs.find(peerName);
    if (it == pingTimesByHostMs.end())
        return pingTimeUnknownHostsMs;
    return it->second;
}

HttpResponse
HttpExchangeConnector::
getResponse(const HttpAuctionHandler & connection,
            const HttpHeader & requestHeader,
            const Auction & auction) const
{
    throw ML::Exception("need to override HttpExchangeConnector::getResponse");
}

HttpResponse
HttpExchangeConnector::
getDroppedAuctionResponse(const HttpAuctionHandler & connection,
                          const Auction & auction,
                          const std::string & reason) const
{
    // Default for when dropped auction == no bid
    return getResponse(connection, connection.header, auction);
}

HttpResponse
HttpExchangeConnector::
getErrorResponse(const HttpAuctionHandler & connection,
                 const Auction & auction,
                 const std::string & errorMessage) const
{
    // Default for when error == no bid
    return getResponse(connection, connection.header, auction);
}

void
HttpExchangeConnector::
handleUnknownRequest(HttpAuctionHandler & connection,
                     const HttpHeader & header,
                     const std::string & payload) const
{
    // Deal with the "/ready" request
    
    if (header.resource == "/ready") {
        connection.putResponseOnWire(HttpResponse(200, "text/plain", "1"));
        return;
    }

    // Otherwise, it's an error

    connection.sendErrorResponse("unknown resource " + header.resource);
}

ExchangeConnector::ExchangeCompatibility
HttpExchangeConnector::
getCampaignCompatibility(const AgentConfig & config,
                         bool includeReasons) const
{
    return ExchangeConnector
        ::getCampaignCompatibility(config, includeReasons);
}

ExchangeConnector::ExchangeCompatibility
HttpExchangeConnector::
getCreativeCompatibility(const Creative & creative,
                         bool includeReasons) const
{
    return ExchangeConnector
        ::getCreativeCompatibility(creative, includeReasons);
}

} // namespace RTBKIT
