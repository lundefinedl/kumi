#include "stdafx.h"

#if WITH_UNPACKED_RESOUCES

#include "utils.hpp"
#include "file_utils.hpp"
#include "resource_manager.hpp"
#include "file_watcher.hpp"
#include "threading.hpp"
#include "logger.hpp"
#include "path_utils.hpp"
#include "graphics.hpp"

using namespace std::tr1::placeholders;
using namespace std;

static bool file_exists(const char *filename)
{
  if (_access(filename, 0) != 0)
    return false;

  struct _stat status;
  if (_stat(filename, &status) != 0)
    return false;

  return !!(status.st_mode & _S_IFREG);
}

static string normalize_path(const std::string &path, bool add_trailing_slash) {
  string res(path);
  boost::replace_all(res, "\\", "/");
  if (add_trailing_slash) {
    if (!res.empty() && res.back() != '/')
      res.push_back('/');
  }

  return res;
}

static ResourceManager *g_instance;

ResourceManager &ResourceManager::instance() {
  KASSERT(g_instance);
  return *g_instance;
}

bool ResourceManager::create(const char *outputFilename) {
  KASSERT(!g_instance);
  g_instance = new ResourceManager(outputFilename);
  return true;
}

bool ResourceManager::close() {
  delete exch_null(g_instance);
  return true;
}

ResourceManager::ResourceManager(const char *outputFilename) 
  : _outputFilename(outputFilename)
{
  _paths.push_back("./");
}

ResourceManager::~ResourceManager() {
  if (!_outputFilename.empty()) {
    FILE *f = fopen(_outputFilename.c_str(), "wt");
    for (auto it = begin(_readFiles); it != end(_readFiles); ++it) {
      fprintf(f, "%s\t%s\n", it->orgName.c_str(), it->resolvedName.c_str());
    }
    fclose(f);
  }
}

void ResourceManager::add_path(const std::string &path) {
  _paths.push_back(normalize_path(path, true));
}

bool ResourceManager::load_file(const char *filename, std::vector<char> *buf) {
  const string &full_path = resolve_filename(filename, true);
  if (full_path.empty()) return false;
  _readFiles.insert(FileInfo(filename, full_path));

  return ::load_file(full_path.c_str(), buf);
}

bool ResourceManager::load_partial(const char *filename, size_t ofs, size_t len, std::vector<char> *buf) {
  buf->resize(len);
  return load_inplace(filename, ofs, len, buf->data());
}

bool ResourceManager::load_inplace(const char *filename, size_t ofs, size_t len, void *buf) {
  const string &full_path = resolve_filename(filename, true);
  _readFiles.insert(FileInfo(filename, full_path));

  ScopedHandle h(CreateFileA(full_path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 
    FILE_ATTRIBUTE_NORMAL, NULL));
  if (h.handle() == INVALID_HANDLE_VALUE)
    return false;

  DWORD size = GetFileSize(h, NULL);
  if (ofs + len > size)
    return false;

  DWORD res;
  if (SetFilePointer(h, ofs, 0, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
    return false;
  }

  if (!ReadFile(h, buf, len, &res, NULL)) {
    DWORD err = GetLastError();
    return false;
  }
  return true;
}

bool ResourceManager::file_exists(const char *filename) {
  return !resolve_filename(filename, false).empty();
}

__time64_t ResourceManager::mdate(const char *filename) {
  struct _stat s;
  _stat(filename, &s);
  return s.st_mtime;

}

string ResourceManager::resolve_filename(const char *filename, bool fullPath) {

  if (::file_exists(filename)) {
    return normalize_path(fullPath ? Path::get_full_path_name(filename) : filename, false);
  }

  auto it = _resolved_paths.find(filename);
  if (it != _resolved_paths.end())
    return it->second;
  string res;
#if _DEBUG
  // warn about duplicates
  int count = 0;
  for (size_t i = 0; i < _paths.size(); ++i) {
    string cand(_paths[i] + filename);
    if (::file_exists(cand.c_str())) {
      count++;
      if (res.empty())
        res = cand.c_str();
    }
  }
  if (count > 1)
    LOG_WARNING_LN("Multiple paths resolved for file: %s", filename);
#else
  for (size_t i = 0; i < _paths.size(); ++i) {
    string cand(_paths[i] + filename);
    if (::file_exists(cand.c_str())) {
      res = cand.c_str();
      break;
    }
  }
#endif
  if (!res.empty()) {
    res = normalize_path(res.c_str(), false);
    _resolved_paths[filename] = res;
  }
  return res;
}

void ResourceManager::add_file_watch(const char *filename, void *token, const cbFileChanged &cb, bool initial_callback, bool *initial_result, int timeout) {
  if (initial_callback) {
    bool res = cb(filename, token);
    if (initial_result)
      *initial_result = res;
  }

  FILE_WATCHER.add_file_watch(filename, token, threading::kMainThread, 
    bind(&ResourceManager::file_changed, this, timeout == -1 ? 2500 : timeout, _1, _2, _3, _4));

  _watched_files[filename].push_back(make_pair(cb, token));
}

void ResourceManager::remove_file_watch(const cbFileChanged &cb) {
  for (auto i = begin(_watched_files); i != end(_watched_files); ++i) {
    auto &v = i->second;
    v.erase(remove_if(begin(v), end(v), [&](const pair<cbFileChanged, void*> &x) { return cb == x.first; }), end(v));
  }
}

void ResourceManager::deferred_file_changed(void *token, FileWatcher::FileEvent event, const string &old_name, const string &new_name) {
  if (0 == --_file_change_ref_count[old_name]) {
    if ((uint32)event & (FileWatcher::kFileEventCreate | FileWatcher::kFileEventModify)) {
      auto i = _watched_files.find(old_name);
      if (i == end(_watched_files))
        return;
      // call all the callbacks registered for the changed file
      auto &v = i->second;
      for (auto j = begin(v); j != end(v); ++j) {
        const pair<cbFileChanged, void*> &x = *j;
        x.first(old_name.c_str(), x.second);
      }
    }
  }
}

void ResourceManager::file_changed(int timeout, void *token, FileWatcher::FileEvent event, const string &old_name, const string &new_name) {
  _file_change_ref_count[old_name]++;
  DISPATCHER.invoke_in(FROM_HERE, threading::kMainThread, timeout,
    bind(&ResourceManager::deferred_file_changed, this, token, event, old_name, new_name));
}

GraphicsObjectHandle ResourceManager::load_texture(const char *filename, const char *friendly_name, bool srgb, D3DX11_IMAGE_INFO *info) {
  string fullPath = resolve_filename(filename, true);
  _readFiles.insert(FileInfo(filename, fullPath));

  return GRAPHICS.load_texture(fullPath.c_str(), friendly_name, srgb, info);
}

#endif