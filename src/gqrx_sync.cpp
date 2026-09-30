#include "gqrx_sync.hpp"

#include <array>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace astra918 {
namespace {

constexpr std::uint64_t kMinimumHz = 70000;
constexpr std::uint64_t kMaximumHz = 260000000;
constexpr std::uint64_t kSampleRate = 120000;
// Gqrx 2.17.7: bw_half = trunc(0.9 * rate / 2), bwh_eff = trunc(0.8 *
// bw_half), and the retune offset = trunc(0.2 * bwh_eff).
constexpr std::uint64_t kRetuneOffsetHz = 8640;
constexpr std::uint64_t kForceDeltaHz = kSampleRate;

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
constexpr int kSendFlags = 0;
void closeSocket(Socket socket) { closesocket(socket); }
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif
void closeSocket(Socket socket) { close(socket); }
#endif

class RemoteSocket {
public:
  explicit RemoteSocket(const std::uint16_t port) {
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
      throw std::runtime_error("Winsock initialization failed");
#endif
    socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_ == kInvalidSocket) {
#ifdef _WIN32
      WSACleanup();
#endif
      throw std::runtime_error("Gqrx remote socket could not be created");
    }
#ifdef __APPLE__
    int noSigpipe = 1;
    setsockopt(socket_, SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe,
               sizeof(noSigpipe));
#endif
#ifdef _WIN32
    DWORD timeout = 500;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char *>(&timeout), sizeof(timeout));
    setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char *>(&timeout), sizeof(timeout));
#else
    timeval timeout{0, 500000};
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(socket_, reinterpret_cast<sockaddr *>(&address),
                sizeof(address)) != 0) {
      closeSocket(socket_);
      socket_ = kInvalidSocket;
#ifdef _WIN32
      WSACleanup();
#endif
      throw std::runtime_error("Gqrx remote control is not listening");
    }
  }

  ~RemoteSocket() {
    if (socket_ != kInvalidSocket)
      closeSocket(socket_);
#ifdef _WIN32
    WSACleanup();
#endif
  }

  RemoteSocket(const RemoteSocket &) = delete;
  RemoteSocket &operator=(const RemoteSocket &) = delete;

  std::string command(const std::string &request) {
    const auto line = request + "\n";
    std::size_t sent = 0;
    while (sent < line.size()) {
      const auto count = send(socket_, line.data() + sent,
                              static_cast<int>(line.size() - sent), kSendFlags);
      if (count <= 0)
        throw std::runtime_error("Gqrx remote write failed");
      sent += static_cast<std::size_t>(count);
    }
    std::string reply;
    while (reply.size() < 128) {
      char byte;
      if (recv(socket_, &byte, 1, 0) != 1)
        throw std::runtime_error("Gqrx remote reply timed out");
      if (byte == '\n')
        return reply;
      reply.push_back(byte);
    }
    throw std::runtime_error("Gqrx remote reply was too long");
  }

private:
  Socket socket_ = kInvalidSocket;
};

} // namespace

GqrxTunePlan planGqrxCenter(const std::uint64_t centerHz) {
  if (centerHz >= kMinimumHz + kForceDeltaHz + kRetuneOffsetHz &&
      centerHz <= kMaximumHz - kRetuneOffsetHz) {
    return {centerHz - kForceDeltaHz - kRetuneOffsetHz,
            centerHz + kRetuneOffsetHz, centerHz + 1};
  }
  if (centerHz >= kMinimumHz + kRetuneOffsetHz &&
      centerHz <= kMaximumHz - kForceDeltaHz - kRetuneOffsetHz) {
    return {centerHz + kForceDeltaHz + kRetuneOffsetHz,
            centerHz - kRetuneOffsetHz, centerHz - 1};
  }
  throw std::out_of_range("Gqrx cannot recenter this frequency through its "
                          "remote API without leaving the tuning range");
}

bool syncGqrxCenter(const std::uint16_t port, const std::uint64_t centerHz,
                    std::string &error) {
  try {
    const auto plan = planGqrxCenter(centerHz);
    RemoteSocket remote(port);
    if (remote.command("_").rfind("Gqrx ", 0) != 0)
      throw std::runtime_error("The remote endpoint is not Gqrx");
    for (const auto frequency : std::array<std::uint64_t, 3>{
             plan.resetHz, plan.centerCommandHz, plan.finalHz}) {
      if (remote.command("F " + std::to_string(frequency)) != "RPRT 0")
        throw std::runtime_error("Gqrx rejected a frequency update");
    }
    if (remote.command("f") != std::to_string(plan.finalHz))
      throw std::runtime_error("Gqrx did not adopt the receiver frequency");
    return true;
  } catch (const std::exception &ex) {
    error = ex.what();
    return false;
  }
}

} // namespace astra918
