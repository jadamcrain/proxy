
#include "ProxySession.h"

#include <proxy/ErrorCodes.h>

#include <easylogging++.h>

#include <sys/epoll.h>
#include <unistd.h>


namespace proxy
{

ProxySession::ProxySession(const EndpointConfig& config_, FileDesc& server_fd_, IParserFactory& factory) :
        m_config(config_),
        m_server_fd(std::move(server_fd_)),
        m_s2cParser(std::move(factory.Create(*this))),
        m_c2sParser(std::move(factory.Create(*this)))
{

}

void ProxySession::OnErrorMsg(const char *fmt, ...)
{
        char buffer[80];
        va_list args;
        va_start(args, fmt);
        snprintf(buffer, 80, fmt, args);
        va_end(args);

        LOG(ERROR) << buffer;
}

void ProxySession::OnDebugMsg(const char *fmt, ...)
{
        char buffer[80];
        va_list args;
        va_start(args, fmt);
        snprintf(buffer, 80, fmt, args);
        va_end(args);

        LOG(DEBUG) << buffer;
}

void ProxySession::QueueWrite(const RSlice& output)
{
        this->m_output_vec.push_back(output);
}

void ProxySession::Run()
{
        std::error_code ec;
        FileDesc client_fd(Connect(ec));

        if(ec)
        {
                LOG(WARNING) << "Error connecting: " << ec.message();
                return;
        }

        // now we have open fd's for both client and server we
        // enter an event loop waiting for data to read from either source

        FileDesc epoll_fd(epoll_create(2));

        if(!epoll_fd.IsValid())
        {
                ec = std::error_code(errno, std::system_category());
                LOG(ERROR) << "Error creating epoll fd: " << ec.message();
                return;
        }

        if(!RegisterForDataAvailable(epoll_fd, m_server_fd, ec) || !RegisterForDataAvailable(epoll_fd, client_fd, ec))
        {
                ec = std::error_code(errno, std::system_category());
                LOG(ERROR) << "Error registering epoll fd: " << ec.message();
                return;
        }

        while(!ec)
        {
                RunOne(epoll_fd, client_fd, ec);
        }
}

bool ProxySession::RunOne(FileDesc& epoll_fd, FileDesc& client_fd, std::error_code &ec)
{
    epoll_event event; // TODO - process more than 1 event at a time?

    int num = epoll_wait(epoll_fd, &event, 1, -1);

    if(num < 0)
    {
            ec = std::error_code(errno, std::system_category());
            return false;
    }

    if(event.events & EPOLLIN)
    {
            if(event.data.fd == client_fd)
            {
                    return Transfer(client_fd, m_server_fd, *m_c2sParser, ec);
            }
            else
            {
                    // otherwise assume source is server id
                    return Transfer(m_server_fd, client_fd, *m_s2cParser, ec);
            }
    }
    else
    {
            ec = Error::EPOLL_SOCKET_ERR;
            return false;
    }
}

bool ProxySession::Transfer(FileDesc& src, FileDesc& dest, IParser& parser, std::error_code &ec)
{
    // until we're using an actual plugin, just read and write to a local buffer
    auto inBuff = parser.GetWriteSlice();
    auto numRead = read(src, inBuff, inBuff.Size());

    if(numRead < 0) {
            ec = std::error_code(errno, std::system_category());
            return false;
    }

    if(numRead == 0) {
            ec = Error::END_OF_FILE;
            return false;
    }

    RSlice readyBytes = inBuff.ToRSlice().Take(numRead);

    // now notify the parser that we wrote some data into its input buffer
    const bool SUCCESS = parser.Parse(readyBytes);

    while(!this->m_output_vec.empty())
    {
        RSlice slice = m_output_vec.front();
        m_output_vec.pop_front();

        while(!slice.IsEmpty())
        {
            auto numWritten = write(dest, slice, slice.Size());

            if (numWritten <= 0) {
                ec = std::error_code(errno, std::system_category());
                return false;
            }

            slice.Advance(numWritten);
        }
    }

    return SUCCESS;
}

bool ProxySession::RegisterForDataAvailable(const FileDesc& epoll_fd, const FileDesc& fd, std::error_code &ec)
{
    return Modify(epoll_fd, EPOLL_CTL_ADD, fd, EPOLLIN, ec);
}

bool ProxySession::Modify(const FileDesc& epoll_fd, int operation, const FileDesc& fd, uint32_t events, std::error_code &ec)
{
    epoll_event evt;
    evt.events = EPOLLIN;
    evt.data.fd = fd;

    if (epoll_ctl(epoll_fd, operation, fd, &evt) < 0)
    {
            ec = std::error_code(errno, std::system_category());
            return false;
    }

    return true;
}

FileDesc ProxySession::Connect(std::error_code& ec)
{
    FileDesc client_fd(socket(AF_INET, SOCK_STREAM, 0));
    if(!client_fd.IsValid())
    {
            ec = std::error_code(errno, std::system_category());
            return FileDesc();
    }

    struct sockaddr_in serveraddr;
    serveraddr.sin_addr.s_addr = m_config.address.s_addr;
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(m_config.port );

    char buffer[INET_ADDRSTRLEN];
    auto address = inet_ntop(AF_INET, &serveraddr.sin_addr, buffer, INET_ADDRSTRLEN);
    LOG(INFO) << "Initiating connection to " << address << ":" << m_config.port;

    auto res = connect(client_fd, (struct sockaddr *)&serveraddr, sizeof(serveraddr));

    if(res < 0)
    {
            ec = std::error_code(errno, std::system_category());
            return FileDesc();
    }

    LOG(INFO) << "Connected to " << address << ":" << m_config.port;

    return client_fd;
}

}
