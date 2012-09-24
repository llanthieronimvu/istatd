
#include "HttpServer.h"
#include "StatServer.h"
#include "istat/strfunc.h"
#include "Logs.h"
#include "Debug.h"


#include <boost/lexical_cast.hpp>
#include <boost/bind.hpp>
#include <ctype.h>
#include <iostream>


using namespace istat;
using boost::asio::ip::tcp;
using namespace boost::asio;


DebugOption debugHttp("http");

HttpServer::HttpServer(int port, boost::asio::io_service &svc, std::string listen_addr) :
    port_(port),
    svc_(svc),
    acceptor_(svc),
    timer_(svc)
{
    if (port_ != 0)
    {
        sInfo_.port = port_;
        tcp::resolver resolver(svc);
        //tcp::resolver::query query("0.0.0.0", boost::lexical_cast<std::string>(port));
        //tcp::endpoint endpoint = *resolver.resolve(query);
        acceptor_.open(tcp::v4());
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
        if (listen_addr.length() > 0) {
            acceptor_.bind(tcp::endpoint(ip::address::from_string(listen_addr.c_str()), port));
        } else {
            acceptor_.bind(tcp::endpoint(tcp::v4(), port));
        }
        acceptor_.listen();
        acceptOne();
    }
}

HttpServer::~HttpServer()
{
}

void HttpServer::getInfo(HttpServerInfo &oInfo)
{
    oInfo = sInfo_;
}

void HttpServer::acceptOne()
{
    LogDebug << "HttpService::acceptOne()";
    boost::shared_ptr<IHttpRequest> sptr(newHttpRequest());
    HttpRequestHolder request(sptr);
    acceptor_.async_accept(request->socket(),
        boost::bind(&HttpServer::handleAccept, this, placeholders::error, request));
}

void HttpServer::handleAccept(boost::system::error_code const &e, HttpRequestHolder const &req)
{
    LogDebug << "HttpService::handleAccept()";
    ++sInfo_.numRequests;
    if (!e)
    {
        if (debugHttp.enabled())
        {
            LogNotice << "http request";
        }
        ++sInfo_.current;
        sInfo_.currentGauge.value((int32_t)sInfo_.current);
        onRequest_(req);
        req->readHeaders();
        acceptOne();
        return;
    }
    ++sInfo_.numErrors;
    LogWarning << "Error accepting a HTTP request: " << e;
    timer_.expires_from_now(boost::posix_time::seconds(1));
    timer_.async_wait(boost::bind(&HttpServer::acceptOne, this));
}

HttpRequest *HttpServer::newHttpRequest()
{
    return new HttpRequest(svc(), this);
}




HttpRequest::HttpRequest(boost::asio::io_service &svc, HttpServer *hs) :
    socket_(svc),
    hs_(hs)
{
}

HttpRequest::~HttpRequest()
{
}

void HttpRequest::readHeaders()
{
    LogSpam << "HttpRequest::readHeaders()";
    if (headerData_.size() > 0)
    {
        throw std::runtime_error("Can't readHeaders() twice!");
    }
    boost::asio::async_read_until(socket_, header_, "\r\n\r\n", 
        boost::bind(&HttpRequest::on_header, HttpRequestHolder(shared_from_this()),
        placeholders::error, placeholders::bytes_transferred));
}

void HttpRequest::readBody()
{
    LogSpam << "HttpRequest::readBody()";
    std::string const *cl = header("Content-Length");
    if (!cl)
    {
        throw std::runtime_error("Can't read body without content-length");
    }
    size_t len = boost::lexical_cast<size_t>(*cl);
    bodyData_.resize(len);
    bodyRead_ = 0;
    if (headerSize_ < headerData_.size() && len > 0)
    {
        size_t toCopy = headerData_.size() - headerSize_;
        if (toCopy > len)
        {
            toCopy = len;
        }
        if (debugHttp.enabled())
        {
            LogNotice << "http toCopy" << toCopy;
        }
        std::copy(&headerData_[headerSize_], &headerData_[headerSize_] + toCopy, &bodyData_[0]);
        bodyRead_ = toCopy;
    }
    if (debugHttp.enabled())
    {
        LogNotice << "http readBody" << len << "bytes";
    }
    try
    {
        size_t toRead = bodyData_.size() - bodyRead_;
        if (toRead == 0)
        {
            if (debugHttp.enabled())
            {
                LogNotice << "http toRead complete";
            }
            on_body(boost::system::errc::make_error_code(boost::system::errc::success), toRead);
            return;
        }
        if (!socket_.is_open())
        {
            throw std::runtime_error("Socket is closed inside readBody()");
        }
        if (debugHttp.enabled())
        {
            LogNotice << "http queue read" << toRead;
        }
        boost::asio::async_read(socket_, boost::asio::buffer(&bodyData_[bodyRead_], toRead), boost::asio::transfer_at_least(toRead),
            boost::bind(&HttpRequest::on_body, HttpRequestHolder(shared_from_this()), placeholders::error, placeholders::bytes_transferred));
    }
    catch (std::exception const &x)
    {
        LogWarning << "exception calling async_read() in readBody():" << x.what();
        throw x;
    }
}

void HttpRequest::on_header(boost::system::error_code const &err, size_t xfer)
{
    LogSpam << "HttpRequest::on_header()";
    if (!!err)
    {
        LogWarning << "HttpRequest::on_header(): " << err;
        error();
        return;
    }
    headerSize_ = xfer;
    std::ostringstream is;
    is << &header_;
    headerData_ = is.str();
    //  parse header
    bool first = true;
    size_t b = 0;
    for (size_t p = 3, n = headerData_.size(); p != n-2; ++p)
    {
        if (headerData_[p] == '\n' && headerData_[p-1] == '\r' && headerData_[p+1] != ' ')
        {
            if (headerData_[p+1] == '\r' && headerData_[p+2] == '\n')
            {
                //found end of headers ... record the final header if it exists
                if (p > b) {
                    parseHeader(std::string(&headerData_[b], &headerData_[p-1]));
                }
                headerSize_ = p + 3;
                break;
            }
            else if (first)
            {
                parseMethod(std::string(&headerData_[b], &headerData_[p-1]));
                first = false;
            }
            else
            {
                parseHeader(std::string(&headerData_[b], &headerData_[p-1]));
            }
            b = p+1;
        }
    }
    if (!version_.size() || !headers_.size() || !headerSize_)
    {
        LogWarning << "Mal-formed HTTP request: method" << method_ << "url" << url_;
        error();
        return;
    }
    //  fire the event
    onHeader_();
    onHeader_.disconnect_all_slots();
}

void HttpRequest::on_body(boost::system::error_code const &err, size_t xfer)
{
    LogSpam << "HttpRequest::on_body()";
    if (!!err)
    {
        LogWarning << "HttpRequest::on_body(): " << err;
        if ((xfer > 0) &&
            (err.category() != boost::asio::error::get_misc_category() ||
                err.value() != boost::asio::error::eof))
        {
            error();
            return;
        }
    }
    if (debugHttp.enabled())
    {
        LogNotice << "http on_body" << xfer << "bytes";
    }
    //  got the body!
    onBody_();
    onBody_.disconnect_all_slots();
    onError_.disconnect_all_slots();
}

boost::shared_ptr<std::list<std::string> > HttpRequest::headers() const
{
    boost::shared_ptr<std::list<std::string> > ret(new std::list<std::string>());
    for (std::map<std::string, std::string>::const_iterator ptr(headers_.begin()), end(headers_.end());
        ptr != end; ++ptr)
    {
        ret->push_back((*ptr).first);
    }
    return ret;
}

std::string const *HttpRequest::header(std::string const &key) const
{
    std::string copy(key);
    munge(copy);
    std::map<std::string, std::string>::const_iterator ptr(headers_.find(copy));
    if (ptr == headers_.end())
    {
        return 0;
    }
    return &(*ptr).second;
}

void HttpRequest::parseMethod(std::string const &data)
{
    std::string temp;
    split(data, ' ', method_, temp);
    trim(temp);
    split(temp, ' ', url_, version_);
    if (debugHttp.enabled())
    {
        LogNotice << "http method" << method_ << "url" << url_ << "version" << version_;
    }
}

void HttpRequest::parseHeader(std::string const &data)
{
    std::string left, right;
    split(data, ':', left, right);
    munge(left);
    trim(right);
    headers_[left] += right;
    LogSpam << "HttpRequest::parseHeader Appending to header " << left << "with data" << right;
}

void HttpRequest::error()
{
    ++hs_->sInfo_.numErrors;
    LogDebug << "HttpRequest::error()";
    onError_();
    onError_.disconnect_all_slots();
    onBody_.disconnect_all_slots();
    onHeader_.disconnect_all_slots();
}

void HttpRequest::appendReply(char const *data, size_t size)
{
    reply_.insert(reply_.end(), data, data+size);
}

void HttpRequest::doReply(int code, std::string const &ctype, std::string const &xheaders)
{
    if (debugHttp.enabled())
    {
        LogNotice << "http reply" << code << ctype;
    }
    else
    {
        LogDebug << "HttpRequest::doReply()";
    }
    if (code >= 400)
    {
        ++hs_->sInfo_.httpErrors;
    }
    std::string headers;

    headers += "HTTP/1.1 ";
    headers += boost::lexical_cast<std::string>(code);
    headers += " (that's a status code)";
    headers += "\r\n";

    headers += "Content-Type: ";
    headers += ctype;
    headers += "\r\n";

    headers += "Content-Length: ";
    headers += boost::lexical_cast<std::string>(reply_.size());
    headers += "\r\n";

    headers += "Connection: close";
    headers += "\r\n";

    headers += xheaders;
    if (debugHttp.enabled() && xheaders.size())
    {
        LogNotice << "http xheaders" << xheaders;
    }

    headers += "\r\n";
    reply_.insert(reply_.begin(), headers.begin(), headers.end());
    boost::asio::async_write(socket_, boost::asio::buffer(&reply_[0], reply_.size()), boost::asio::transfer_all(),
        boost::bind(&HttpRequest::on_reply, HttpRequestHolder(shared_from_this()), placeholders::error, placeholders::bytes_transferred));
    onHeader_.disconnect_all_slots();
    onBody_.disconnect_all_slots();
}

void HttpRequest::on_reply(boost::system::error_code const &err, size_t xfer)
{
    if (debugHttp.enabled())
    {
        LogNotice << "http on_reply() complete" << err;
    }
    LogDebug << "HttpRequest::on_reply()";
    if (!!err)
    {
        ++hs_->sInfo_.numErrors;
    }
    assert(hs_->sInfo_.current > 0);
    --hs_->sInfo_.current;
    hs_->sInfo_.currentGauge.value((int32_t)hs_->sInfo_.current);
    //  this should soon go away, as the stack reference will go away!
    socket_.close();
    onError_.disconnect_all_slots();
}

