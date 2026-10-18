#include "http_transport.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <mutex>

namespace kugelaudio::detail {

namespace {

void global_init() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::size_t write_body(char* data, std::size_t size, std::size_t count, void* user) {
  static_cast<std::string*>(user)->append(data, size * count);
  return size * count;
}

std::string lowercase(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

std::string trim(const std::string& text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  const auto end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

std::size_t write_header(char* data, std::size_t size, std::size_t count, void* user) {
  auto* headers = static_cast<std::map<std::string, std::string>*>(user);
  const std::string line(data, size * count);
  if (line.rfind("HTTP/", 0) == 0) {
    headers->clear();  // a new response (100-continue, redirect): forget the last one
    return size * count;
  }
  const auto colon = line.find(':');
  if (colon != std::string::npos) {
    (*headers)[lowercase(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
  }
  return size * count;
}

struct EasyDeleter {
  void operator()(CURL* easy) const { curl_easy_cleanup(easy); }
};
struct SlistDeleter {
  void operator()(curl_slist* list) const { curl_slist_free_all(list); }
};
struct MimeDeleter {
  void operator()(curl_mime* mime) const { curl_mime_free(mime); }
};

HttpResponse run(const HttpRequest& request, CURLSH* share) {
  global_init();
  std::unique_ptr<CURL, EasyDeleter> easy(curl_easy_init());
  if (!easy) throw TransportError("Could not initialise libcurl.", false);
  CURL* h = easy.get();

  HttpResponse response;
  std::unique_ptr<curl_slist, SlistDeleter> headers;
  auto add_header = [&headers](const std::string& line) {
    curl_slist* next = curl_slist_append(headers.get(), line.c_str());
    if (next == nullptr) throw TransportError("Could not allocate request headers.", false);
    static_cast<void>(headers.release());
    headers.reset(next);
  };
  for (const auto& [name, value] : request.headers) add_header(name + ": " + value);
  // Never wait for "100 Continue" before sending a body.
  add_header("Expect:");

  std::unique_ptr<curl_mime, MimeDeleter> mime;
  if (!request.multipart.empty()) {
    mime.reset(curl_mime_init(h));
    for (const auto& part : request.multipart) {
      curl_mimepart* field = curl_mime_addpart(mime.get());
      curl_mime_name(field, part.name.c_str());
      curl_mime_data(field, part.data.data(), part.data.size());
      if (part.filename) curl_mime_filename(field, part.filename->c_str());
      if (!part.content_type.empty()) curl_mime_type(field, part.content_type.c_str());
    }
    curl_easy_setopt(h, CURLOPT_MIMEPOST, mime.get());
  } else if (request.json_body) {
    add_header("Content-Type: application/json; charset=utf-8");
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(request.json_body->size()));
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, request.json_body->c_str());
  }
  if (request.method == "POST" && request.multipart.empty() && !request.json_body) {
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, 0L);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, "");
  }
  if (request.method != "GET" && request.method != "POST") {
    curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, request.method.c_str());
  }

  const long timeout_ms = static_cast<long>(request.timeout.count());
  const long timeout_s = std::max(1L, (timeout_ms + 999) / 1000);
  curl_easy_setopt(h, CURLOPT_URL, request.url.c_str());
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
  // Like an idle read timeout: abort when no byte arrives for `timeout`,
  // without capping a long but progressing download.
  curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, timeout_s);
  curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_body);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &response.body);
  curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, write_header);
  curl_easy_setopt(h, CURLOPT_HEADERDATA, &response.headers);
  if (share != nullptr) curl_easy_setopt(h, CURLOPT_SHARE, share);

  std::array<char, CURL_ERROR_SIZE> error{};
  curl_easy_setopt(h, CURLOPT_ERRORBUFFER, error.data());

  const CURLcode code = curl_easy_perform(h);
  if (code != CURLE_OK) {
    const std::string detail = error[0] != '\0' ? error.data() : curl_easy_strerror(code);
    throw TransportError(detail, code == CURLE_OPERATION_TIMEDOUT);
  }
  long status = 0;
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
  response.status = static_cast<int>(status);
  return response;
}

}  // namespace

struct HttpTransport::Share {
  CURLSH* handle = nullptr;
  std::array<std::mutex, CURL_LOCK_DATA_LAST> locks;

  static void lock(CURL*, curl_lock_data data, curl_lock_access, void* user) {
    static_cast<Share*>(user)->locks[static_cast<std::size_t>(data)].lock();
  }
  static void unlock(CURL*, curl_lock_data data, void* user) {
    static_cast<Share*>(user)->locks[static_cast<std::size_t>(data)].unlock();
  }
};

HttpTransport::HttpTransport() : share_(std::make_unique<Share>()) {
  global_init();
  share_->handle = curl_share_init();
  if (share_->handle != nullptr) {
    curl_share_setopt(share_->handle, CURLSHOPT_LOCKFUNC, &Share::lock);
    curl_share_setopt(share_->handle, CURLSHOPT_UNLOCKFUNC, &Share::unlock);
    curl_share_setopt(share_->handle, CURLSHOPT_USERDATA, share_.get());
    curl_share_setopt(share_->handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(share_->handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    curl_share_setopt(share_->handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
  }
}

HttpTransport::~HttpTransport() {
  if (share_->handle != nullptr) curl_share_cleanup(share_->handle);
}

HttpResponse HttpTransport::perform(const HttpRequest& request) {
  return run(request, share_->handle);
}

HttpResponse perform_once(const HttpRequest& request) { return run(request, nullptr); }

}  // namespace kugelaudio::detail
