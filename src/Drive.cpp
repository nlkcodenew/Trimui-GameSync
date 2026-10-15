// Drive.cpp — P3: Google Drive API v3 client (API key, no OAuth).
// JSON via nlohmann/json (MIT, src/thirdparty/json.hpp). HTTPS via libcurl
// with the bundled certs/cacert.pem (devices have no system CA store).

#include "Drive.h"

#include "AppConfig.h"
#include "thirdparty/json.hpp"

#include <curl/curl.h>

#include <openssl/evp.h>
#include <strings.h>
#include <sys/stat.h>

#include <cerrno>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace GameSync {

namespace {

size_t curlWriteCb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *s = static_cast<std::string *>(userdata);
  s->append(ptr, size * nmemb);
  return size * nmemb;
}

std::string trimLocal(std::string s) {
  size_t b = 0;
  while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' ||
                          s[b] == '\n'))
    ++b;
  size_t e = s.size();
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' ||
                   s[e - 1] == '\n'))
    --e;
  return s.substr(b, e - b);
}

bool isIdChar(char ch) {
  const unsigned char u = static_cast<unsigned char>(ch);
  return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
         (u >= '0' && u <= '9') || ch == '-' || ch == '_';
}

void putU32(std::string &o, uint32_t v) {
  o.push_back(static_cast<char>(v & 0xFF));
  o.push_back(static_cast<char>((v >> 8) & 0xFF));
  o.push_back(static_cast<char>((v >> 16) & 0xFF));
  o.push_back(static_cast<char>((v >> 24) & 0xFF));
}

void putU64(std::string &o, uint64_t v) {
  for (int i = 0; i < 8; ++i) o.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void putStr(std::string &o, const std::string &s) {
  putU32(o, static_cast<uint32_t>(s.size()));
  o.append(s);
}

bool getU32(const char *&p, const char *end, uint32_t &v) {
  if (end - p < 4) return false;
  v = static_cast<uint32_t>(static_cast<unsigned char>(p[0])) |
      (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
      (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
      (static_cast<uint32_t>(static_cast<unsigned char>(p[3])) << 24);
  p += 4;
  return true;
}

bool getU64(const char *&p, const char *end, uint64_t &v) {
  if (end - p < 8) return false;
  v = 0;
  for (int i = 0; i < 8; ++i)
    v |= static_cast<uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  p += 8;
  return true;
}

bool getStr(const char *&p, const char *end, std::string &s) {
  uint32_t len = 0;
  if (!getU32(p, end, len)) return false;
  if (static_cast<uint64_t>(end - p) < len) return false;
  s.assign(p, len);
  p += len;
  return true;
}

const char kDriveMagic[8] = {'G', 'S', 'D', 'R', 'V', '0', '0', '1'};

std::string jsonStr(const nlohmann::json &j, const char *key) {
  const auto it = j.find(key);
  if (it != j.end() && it->is_string()) return it->get<std::string>();
  return "";
}

}  // namespace

std::string parseDriveFolderId(std::string s) {
  s = trimLocal(s);
  const char *markers[] = {"/folders/", "open?id=", "id="};
  for (const char *mk : markers) {
    const size_t p = s.find(mk);
    if (p != std::string::npos) {
      s = s.substr(p + strlen(mk));
      break;
    }
  }
  const size_t e = s.find_first_of("/?&#");
  if (e != std::string::npos) s = s.substr(0, e);
  if (s.size() < 10) return "";
  for (char ch : s) {
    if (!isIdChar(ch)) return "";
  }
  return s;
}

std::string resolveDriveApiKey() {
  const char *env = getenv("GS_DRIVE_API_KEY");
  if (env && env[0] != '\0') return std::string(env);
  return AppConfig::instance().getSetting("drive_api_key");
}

DriveClient::DriveClient(const std::string &apiKey, const std::string &appDir)
    : apiKey_(apiKey), caBundle_(appDir + "/certs/cacert.pem") {}

std::string DriveClient::urlEncode(const std::string &s) {
  CURL *h = curl_easy_init();
  if (!h) return s;
  char *e = curl_easy_escape(h, s.c_str(), static_cast<int>(s.size()));
  std::string out = (e != nullptr) ? e : s;
  if (e) curl_free(e);
  curl_easy_cleanup(h);
  return out;
}

bool DriveClient::httpGet(const std::string &url, std::string &body,
                          long &httpCode, std::string &curlErr) {
  body.clear();
  httpCode = 0;
  curlErr.clear();
  CURL *h = curl_easy_init();
  if (!h) {
    curlErr = "curl_easy_init failed";
    return false;
  }
  char errbuf[CURL_ERROR_SIZE];
  errbuf[0] = '\0';
  curl_easy_setopt(h, CURLOPT_URL, url.c_str());
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, curlWriteCb);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(h, CURLOPT_USERAGENT, "Trimui-GameSync/1.0");
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(h, CURLOPT_TIMEOUT, 90L);
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(h, CURLOPT_MAXREDIRS, 3L);
  curl_easy_setopt(h, CURLOPT_CAINFO, caBundle_.c_str());
  curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
  // NOTE: SSL verification stays ON (never set CURLOPT_SSL_VERIFYPEER 0).
  const CURLcode rc = curl_easy_perform(h);
  if (rc == CURLE_OK) {
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &httpCode);
  } else {
    curlErr = (errbuf[0] != '\0') ? errbuf : curl_easy_strerror(rc);
  }
  curl_easy_cleanup(h);
  return rc == CURLE_OK;
}

bool DriveClient::listChildren(const std::string &folderId,
                               std::vector<DriveItem> &out, DriveError &err) {
  out.clear();
  err = DriveError{};
  if (apiKey_.empty()) {
    err.message =
        "Thieu API key Drive (env GS_DRIVE_API_KEY hoac Cai dat).";
    return false;
  }
  const std::string query = "'" + folderId + "' in parents and trashed=false";
  const std::string fields =
      "files(id,name,mimeType,size,md5Checksum,modifiedTime),nextPageToken";
  const std::string base = "https://www.googleapis.com/drive/v3/files?q=" +
                           urlEncode(query) + "&fields=" + urlEncode(fields) +
                           "&pageSize=1000&key=" + urlEncode(apiKey_);
  std::string pageToken;
  for (;;) {
    std::string url = base;
    if (!pageToken.empty()) url += "&pageToken=" + urlEncode(pageToken);
    std::string body;
    long httpCode = 0;
    std::string curlErr;
    if (!httpGet(url, body, httpCode, curlErr)) {
      err.httpCode = 0;
      err.message = "Loi mang: " + curlErr;
      return false;
    }
    nlohmann::json j;
    try {
      j = nlohmann::json::parse(body);
    } catch (const std::exception &e) {
      err.httpCode = static_cast<int>(httpCode);
      err.message = std::string("JSON loi: ") + e.what();
      return false;
    }
    if (j.contains("error")) {
      err.httpCode = static_cast<int>(httpCode);
      err.message = "Drive API: " + jsonStr(j["error"], "message");
      if (err.message == "Drive API: ") err.message = "Drive API bao loi.";
      return false;
    }
    if (httpCode != 200) {
      err.httpCode = static_cast<int>(httpCode);
      err.message = "HTTP " + std::to_string(httpCode);
      return false;
    }
    const auto fit = j.find("files");
    if (fit != j.end() && fit->is_array()) {
      for (const auto &f : *fit) {
        if (!f.is_object()) continue;
        DriveItem it;
        it.id = jsonStr(f, "id");
        it.name = jsonStr(f, "name");
        it.mimeType = jsonStr(f, "mimeType");
        it.isFolder = (it.mimeType == "application/vnd.google-apps.folder");
        const std::string sz = jsonStr(f, "size");
        if (!sz.empty()) {
          try {
            it.size = std::stoull(sz);
          } catch (const std::exception &) {
            it.size = 0;
          }
        }
        it.md5 = jsonStr(f, "md5Checksum");
        it.mtime = jsonStr(f, "modifiedTime");
        if (!it.id.empty()) out.push_back(std::move(it));
      }
    }
    pageToken = jsonStr(j, "nextPageToken");
    if (pageToken.empty()) break;
  }
  return true;
}

bool DriveClient::listRecursive(const std::string &rootFolderId,
                                const std::map<std::string, std::string> &systemMap,
                                std::vector<DriveFile> &out,
                                std::atomic<uint64_t> &progress,
                                const std::atomic<bool> &cancel,
                                DriveError &err) {
  out.clear();
  err = DriveError{};
  struct Frame {
    std::string id;
    std::string relPrefix;
    std::string topFolder;
  };
  std::vector<Frame> stack;
  stack.push_back(Frame{rootFolderId, "", ""});
  while (!stack.empty() && !cancel) {
    Frame fr = std::move(stack.back());
    stack.pop_back();
    std::vector<DriveItem> kids;
    DriveError cerr;
    if (!listChildren(fr.id, kids, cerr)) {
      err = cerr;
      return false;
    }
    for (const auto &k : kids) {
      if (cancel) break;
      if (k.isFolder) {
        if (k.name == "PortMaster") continue;  // out of app scope (defense in depth)
        const std::string top = fr.topFolder.empty() ? k.name : fr.topFolder;
        stack.push_back(Frame{k.id, fr.relPrefix + k.name + "/", top});
      } else {
        DriveFile df;
        df.relPath = fr.relPrefix + k.name;
        df.driveFolder = fr.topFolder;
        const auto mit = systemMap.find(fr.topFolder);
        df.system = (mit != systemMap.end()) ? mit->second : "";
        df.id = k.id;
        df.size = k.size;
        df.md5 = k.md5;
        df.mtime = k.mtime;
        out.push_back(std::move(df));
        ++progress;
      }
    }
  }
  if (cancel) {
    err.message = "Đã hủy bởi người dùng.";
    return false;
  }
  std::sort(out.begin(), out.end(), [](const DriveFile &a, const DriveFile &b) {
    return a.relPath < b.relPath;
  });
  return true;
}

bool loadSystemMap(const std::string &path,
                   std::map<std::string, std::string> &out) {
  out.clear();
  std::ifstream f(path);
  if (!f) return false;
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(f);
  } catch (const std::exception &) {
    return false;
  }
  const auto mit = j.find("mapping");
  if (mit == j.end() || !mit->is_object()) return false;
  for (const auto &kv : mit->items()) {
    if (kv.key().empty() || kv.key()[0] == '_') continue;
    if (kv.value().is_string()) out[kv.key()] = kv.value().get<std::string>();
  }
  return true;
}

bool saveDriveCache(const std::string &path,
                    const std::vector<DriveFile> &files) {
  std::string o;
  o.append(kDriveMagic, 8);
  putU64(o, static_cast<uint64_t>(files.size()));
  for (const auto &f : files) {
    putStr(o, f.relPath);
    putStr(o, f.driveFolder);
    putStr(o, f.system);
    putStr(o, f.id);
    putU64(o, f.size);
    putStr(o, f.md5);
    putStr(o, f.mtime);
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(o.data(), static_cast<std::streamsize>(o.size()));
  return static_cast<bool>(out);
}

bool loadDriveCache(const std::string &path, std::vector<DriveFile> &files) {
  files.clear();
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::string data((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  const char *p = data.data();
  const char *end = p + data.size();
  if (static_cast<size_t>(end - p) < 8) return false;
  if (memcmp(p, kDriveMagic, 8) != 0) return false;
  p += 8;
  uint64_t count = 0;
  if (!getU64(p, end, count)) return false;
  files.reserve(static_cast<size_t>(count > 1000000 ? 1000000 : count));
  for (uint64_t i = 0; i < count; ++i) {
    DriveFile f;
    if (!getStr(p, end, f.relPath)) return false;
    if (!getStr(p, end, f.driveFolder)) return false;
    if (!getStr(p, end, f.system)) return false;
    if (!getStr(p, end, f.id)) return false;
    if (!getU64(p, end, f.size)) return false;
    if (!getStr(p, end, f.md5)) return false;
    if (!getStr(p, end, f.mtime)) return false;
    files.push_back(std::move(f));
  }
  return p == end;
}

bool driveCacheFileCount(const std::string &path, uint64_t &count) {
  count = 0;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  char magic[8];
  in.read(magic, 8);
  if (!in || memcmp(magic, kDriveMagic, 8) != 0) return false;
  char cnt[8];
  in.read(cnt, 8);
  if (!in) return false;
  const char *p = cnt;
  return getU64(p, cnt + 8, count);
}

std::string driveCachePath(const std::string &dataDir,
                           const std::string &folderId) {
  return dataDir + "/drive_cache_" + folderId + ".bin";
}

}  // namespace GameSync


// ---------------- P4: file download with resume + md5 verify ----------------

namespace GameSync {
namespace {

struct DlCtx {
  FILE *fp = nullptr;
  uint64_t offset = 0;  // resume offset (bytes already in .part)
  uint64_t done = 0;    // offset + bytes written this session
  uint64_t total = 0;   // offset + Content-Length (0 = unknown)
  std::function<bool(uint64_t, uint64_t)> progress;  // false = abort
  bool aborted = false;
};

size_t dlWriteCb(char *ptr, size_t size, size_t nmemb, void *ud) {
  DlCtx *c = static_cast<DlCtx *>(ud);
  const size_t n = size * nmemb;
  if (fwrite(ptr, 1, n, c->fp) != n) return 0;  // disk error -> abort
  c->done += n;
  if (c->progress && !c->progress(c->done, c->total)) {
    c->aborted = true;
    return 0;
  }
  return n;
}

size_t dlHeaderCb(char *ptr, size_t size, size_t nmemb, void *ud) {
  DlCtx *c = static_cast<DlCtx *>(ud);
  const size_t n = size * nmemb;
  if (n > 15 && strncasecmp(ptr, "Content-Length:", 15) == 0) {
    const uint64_t cl = static_cast<uint64_t>(strtoull(ptr + 15, nullptr, 10));
    c->total = c->offset + cl;
  }
  return n;
}

bool dlMkdirs(const std::string &path) {
  if (path.empty()) return false;
  std::string cur;
  for (size_t i = 0; i < path.size(); ++i) {
    cur.push_back(path[i]);
    if (path[i] == '/' && cur.size() > 1) {
      if (mkdir(cur.c_str(), 0777) != 0 && errno != EEXIST) return false;
    }
  }
  if (mkdir(cur.c_str(), 0777) != 0 && errno != EEXIST) return false;
  return true;
}

std::string md5OfFile(const std::string &path, bool &ok) {
  ok = false;
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return "";
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  std::string out;
  if (ctx && EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) == 1) {
    char buf[65536];
    size_t n;
    bool fail = false;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
      if (EVP_DigestUpdate(ctx, buf, n) != 1) {
        fail = true;
        break;
      }
    }
    if (!fail) {
      unsigned char dg[EVP_MAX_MD_SIZE];
      unsigned int len = 0;
      if (EVP_DigestFinal_ex(ctx, dg, &len) == 1) {
        static const char *hex = "0123456789abcdef";
        out.reserve(len * 2);
        for (unsigned int i = 0; i < len; ++i) {
          out.push_back(hex[dg[i] >> 4]);
          out.push_back(hex[dg[i] & 15]);
        }
        ok = true;
      }
    }
  }
  if (ctx) EVP_MD_CTX_free(ctx);
  fclose(f);
  return out;
}

bool sameHex(const std::string &a, const std::string &b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
      return false;
  return true;
}

}  // namespace

// One attempt of the download. On success returns true (file renamed to
std::string DriveClient::apiBase() const {
  const char *e = getenv("GS_DRIVE_BASE_URL");
  if (e && e[0] != '\0') {
    std::string b(e);
    while (!b.empty() && b.back() == '/') b.pop_back();
    return b;
  }
  return "https://www.googleapis.com";
}

// destPath). serverIgnoredRange is set when we asked for a range but got 200.
bool DriveClient::downloadOnce(const std::string &fileId,
                              const std::string &destPath,
                              const std::string &expectedMd5, uint64_t offset,
                              const std::function<bool(uint64_t, uint64_t)> &progress,
                              const std::atomic<bool> &cancel, DriveError &err,
                              bool &serverIgnoredRange) {
  serverIgnoredRange = false;
  const std::string partPath = destPath + ".part";
  FILE *fp = fopen(partPath.c_str(), offset ? "ab" : "wb");
  if (!fp) {
    err.message = "Không mở được file tạm để ghi";
    return false;
  }
  DlCtx ctx;
  ctx.fp = fp;
  ctx.offset = offset;
  ctx.done = offset;
  ctx.progress = progress;

  CURL *h = curl_easy_init();
  if (!h) {
    fclose(fp);
    err.message = "curl_easy_init failed";
    return false;
  }
  const std::string url = apiBase() + "/drive/v3/files/" + fileId +
                          "?alt=media&key=" + apiKey_;
  char errbuf[CURL_ERROR_SIZE];
  errbuf[0] = '\0';
  curl_easy_setopt(h, CURLOPT_URL, url.c_str());
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, dlWriteCb);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &ctx);
  curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, dlHeaderCb);
  curl_easy_setopt(h, CURLOPT_HEADERDATA, &ctx);
  if (offset > 0)
    curl_easy_setopt(h, CURLOPT_RANGE,
                     (std::to_string(offset) + "-").c_str());
  curl_easy_setopt(h, CURLOPT_USERAGENT, "Trimui-GameSync/1.0");
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(h, CURLOPT_TIMEOUT, 0L);  // large files: no total timeout
  curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1024L);
  curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 60L);
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(h, CURLOPT_MAXREDIRS, 3L);
  curl_easy_setopt(h, CURLOPT_CAINFO, caBundle_.c_str());
  curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);

  const CURLcode rc = curl_easy_perform(h);
  long httpCode = 0;
  if (rc == CURLE_OK)
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &httpCode);
  fclose(fp);
  curl_easy_cleanup(h);

  if (rc != CURLE_OK) {
    if (ctx.aborted || cancel.load()) {
      err.httpCode = -1;
      err.message = "Đã hủy (giữ file .part để tiếp tục sau)";
    } else {
      err.httpCode = static_cast<int>(httpCode);
      err.message = std::string("Lỗi mạng: ") +
                    ((errbuf[0] != '\0') ? errbuf : curl_easy_strerror(rc)) +
                    " (giữ file .part để tiếp tục sau)";
    }
    return false;
  }
  if (httpCode == 200 && offset > 0) {
    // Server ignored Range: restart from scratch once.
    serverIgnoredRange = true;
    return false;
  }
  if (httpCode == 416 && offset > 0) {
    // Range not satisfiable: .part is probably already complete.
    if (!expectedMd5.empty()) {
      bool ok = false;
      const std::string got = md5OfFile(partPath, ok);
      if (ok && sameHex(got, expectedMd5)) {
        if (rename(partPath.c_str(), destPath.c_str()) == 0) return true;
        err.message = "Không đổi tên file sau khi tải";
        return false;
      }
    }
    remove(partPath.c_str());
    err.httpCode = 416;
    err.message = "File tạm không khớp (đã xóa, tải lại từ đầu ở lần sau)";
    return false;
  }
  if (httpCode != 200 && httpCode != 206) {
    err.httpCode = static_cast<int>(httpCode);
    err.message = "Drive trả HTTP " + std::to_string(httpCode) +
                  (httpCode == 401 || httpCode == 403
                       ? " — kiểm tra API key trong Cài đặt"
                       : " (giữ file .part để tiếp tục sau)");
    return false;
  }
  if (!expectedMd5.empty()) {
    bool ok = false;
    const std::string got = md5OfFile(partPath, ok);
    if (!ok || !sameHex(got, expectedMd5)) {
      remove(partPath.c_str());
      err.message = "MD5 không khớp (file lỗi — đã xóa để tải lại)";
      return false;
    }
  }
  if (rename(partPath.c_str(), destPath.c_str()) != 0) {
    err.message = "Không đổi tên file sau khi tải";
    return false;
  }
  return true;
}

bool DriveClient::download(
    const std::string &fileId, const std::string &destPath,
    const std::string &expectedMd5,
    const std::function<bool(uint64_t, uint64_t)> &progress,
    const std::atomic<bool> &cancel, DriveError &err) {
  err = DriveError{};
  if (fileId.empty() || destPath.empty()) {
    err.message = "Tham số rỗng";
    return false;
  }
  for (char ch : fileId) {
    if (!isIdChar(ch)) {
      err.message = "Drive file id không hợp lệ";
      return false;
    }
  }
  const size_t slash = destPath.rfind('/');
  if (slash != std::string::npos) {
    if (!dlMkdirs(destPath.substr(0, slash))) {
      err.message = "Không tạo được thư mục đích";
      return false;
    }
  }
  uint64_t offset = 0;
  struct stat st;
  if (stat((destPath + ".part").c_str(), &st) == 0 && st.st_size > 0)
    offset = static_cast<uint64_t>(st.st_size);

  bool ignoredRange = false;
  if (downloadOnce(fileId, destPath, expectedMd5, offset, progress, cancel, err,
                   ignoredRange))
    return true;
  if (ignoredRange && !cancel.load()) {
    // Retry once from scratch.
    remove((destPath + ".part").c_str());
    err = DriveError{};
    return downloadOnce(fileId, destPath, expectedMd5, 0, progress, cancel, err,
                        ignoredRange);
  }
  return false;
}

}  // namespace GameSync
