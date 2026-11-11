#include "file_io.h"
#include "parallel.h"
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace slpz {
namespace fs = std::filesystem;
namespace {
class Fd {
  int value_ = -1;

public:
  explicit Fd(int value = -1) : value_(value) {}
  ~Fd() {
    if (value_ >= 0) {
      ::close(value_);
    }
  }
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
  Fd(Fd &&other) noexcept : value_(std::exchange(other.value_, -1)) {}
  Fd &operator=(Fd &&other) noexcept {
    std::swap(value_, other.value_);
    return *this;
  }
  int get() const { return value_; }
  void close_checked() {
    auto fd = std::exchange(value_, -1);
    if (::close(fd) != 0) {
      fail("cannot close output file");
    }
  }
};
[[noreturn]] void io_error(const std::string &operation) {
  fail(operation + ": " + std::strerror(errno));
}
Fd descend(int parent, const std::string &name, bool create) {
  int fd = ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0 && errno == ENOENT && create) {
    if (::mkdirat(parent, name.c_str(), 0755) != 0 && errno != EEXIST) {
      io_error("cannot create directory " + name);
    }
    fd = ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  }
  if (fd < 0) {
    io_error("cannot safely open directory " + name);
  }
  return Fd(fd);
}
Fd open_directory(const fs::path &path, bool create) {
  // Resolve the explicitly chosen root once (e.g. macOS /var -> /private/var).
  // Descendants from archive names are opened only with openat/O_NOFOLLOW.
  const auto resolved = fs::weakly_canonical(fs::absolute(path.empty() ? fs::path(".") : path));
  Fd current(::open(resolved.is_absolute() ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (current.get() < 0) {
    io_error("cannot open output root");
  }
  for (const auto &part : resolved) {
    auto name = part.string();
    if (name == "/" || name == "." || name.empty()) {
      continue;
    }
    current = descend(current.get(), name, create);
  }
  return current;
}
void atomic_write(int parent, const std::string &name, View data, Overwrite policy) {
  struct stat status;
  if (::fstatat(parent, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0) {
    if (policy != Overwrite::Replace) {
      fail("output exists: " + name);
    }
    require(S_ISREG(status.st_mode), "output is not a regular file");
  } else if (errno != ENOENT) {
    io_error("cannot inspect output " + name);
  }
  static std::atomic<uint64_t> sequence{0};
  std::string temporary;
  Fd output;
  for (unsigned attempt = 0; attempt < 128; ++attempt) {
    temporary = ".slpz-tmp-" + std::to_string(::getpid()) + "-" + std::to_string(sequence++);
    int fd = ::openat(parent, temporary.c_str(),
                      O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd >= 0) {
      output = Fd(fd);
      break;
    }
    if (errno != EEXIST) {
      io_error("cannot create temporary output");
    }
  }
  require(output.get() >= 0, "cannot allocate temporary output name");
  struct Cleanup {
    int parent;
    const std::string &name;
    ~Cleanup() { ::unlinkat(parent, name.c_str(), 0); }
  } cleanup{parent, temporary};
  while (!data.empty()) {
    auto count = std::min<size_t>(data.size(), 1u << 30);
    ssize_t written = ::write(output.get(), data.data(), count);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      io_error("cannot write output " + name);
    }
    data = data.subspan(size_t(written));
  }
  output.close_checked();
  if (policy == Overwrite::Replace) {
    if (::renameat(parent, temporary.c_str(), parent, name.c_str()) != 0) {
      io_error("cannot replace output " + name);
    }
  } else if (::linkat(parent, temporary.c_str(), parent, name.c_str(), 0) != 0) {
    io_error("cannot install output " + name);
  }
}
} // namespace

Bytes read_file(const fs::path &path, uint64_t limit) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    fail("cannot open " + path.string());
  }
  auto length = input.tellg();
  require(length >= 0 && uint64_t(length) <= limit && uint64_t(length) <= SIZE_MAX,
          "input exceeds size limit or cannot be sized");
  Bytes bytes{};
  bytes.resize(size_t(length));
  input.seekg(0);
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(bytes.size()));
  }
  if (!input) {
    fail("short read on " + path.string());
  }
  return bytes;
}
void write_file(const fs::path &path, View data, Overwrite policy) {
  auto parent = open_directory(path.parent_path(), false);
  auto name = archive_name(path.filename().string());
  atomic_write(parent.get(), name, data, policy);
}
std::string source_name(const fs::path &path) {
  auto normalized = path.lexically_normal();
  if (normalized.is_absolute() || (!normalized.empty() && *normalized.begin() == "..")) {
    normalized = normalized.filename();
  }
  std::string name = normalized.generic_string();
  bool parent = false;
  for (const auto &part : path) {
    parent |= part == "..";
  }
  if (!path.is_absolute() && !parent) {
    name = path.generic_string();
  }
  archive_name(name); // validate, preserving harmless ./ prefixes verbatim
  return name;
}

struct ExtractionWriter::Impl {
  Fd root;
  Overwrite policy;
  std::mutex mutex;
  std::condition_variable ready, space;
  std::deque<DecodedFile> queue;
  uint64_t queued_bytes = 0;
  bool done = false;
  std::exception_ptr error;
  std::vector<JoiningThread> writers;
  Impl(const fs::path &directory, Overwrite overwrite)
      : root(open_directory(directory, true)), policy(overwrite) {
    try {
      for (unsigned i = 0; i < 3; ++i) {
        writers.emplace_back([this] { worker(); });
      }
    } catch (...) {
      cancel();
      throw;
    }
  }
  void cancel() noexcept {
    {
      std::lock_guard lock(mutex);
      done = true;
      queue.clear();
      queued_bytes = 0;
    }
    ready.notify_all();
    space.notify_all();
    writers.clear();
  }
  ~Impl() { cancel(); }
  void write(DecodedFile file) {
    auto name = archive_name(file.name);
    Fd parent(::dup(root.get()));
    if (parent.get() < 0) {
      io_error("cannot duplicate output directory");
    }
    size_t at = 0;
    for (size_t slash; (slash = name.find('/', at)) != name.npos; at = slash + 1) {
      parent = descend(parent.get(), name.substr(at, slash - at), true);
    }
    atomic_write(parent.get(), name.substr(at), file.bytes, policy);
  }
  void worker() noexcept {
    try {
      for (;;) {
        DecodedFile file;
        {
          std::unique_lock lock(mutex);
          ready.wait(lock, [&] { return done || !queue.empty(); });
          if (queue.empty()) {
            return;
          }
          file = std::move(queue.front());
          queue.pop_front();
          queued_bytes -= file.bytes.size();
        }
        space.notify_all();
        write(std::move(file));
      }
    } catch (...) {
      {
        std::lock_guard lock(mutex);
        if (!error) {
          error = std::current_exception();
        }
        done = true;
        queue.clear();
        queued_bytes = 0;
      }
      ready.notify_all();
      space.notify_all();
    }
  }
  void submit(DecodedFile file) {
    std::unique_lock lock(mutex);
    space.wait(lock, [&] {
      return done || (queue.size() < 48 &&
                      (queue.empty() || queued_bytes + file.bytes.size() <= (256ull << 20)));
    });
    if (error) {
      std::rethrow_exception(error);
    }
    require(!done, "output writer already closed");
    queued_bytes += file.bytes.size();
    queue.push_back(std::move(file));
    ready.notify_one();
  }
  void finish() {
    {
      std::lock_guard lock(mutex);
      done = true;
    }
    ready.notify_all();
    space.notify_all();
    writers.clear();
    if (error) {
      std::rethrow_exception(error);
    }
  }
};
ExtractionWriter::ExtractionWriter(const fs::path &root, Overwrite policy)
    : impl_(std::make_unique<Impl>(root, policy)) {}
ExtractionWriter::~ExtractionWriter() = default;
void ExtractionWriter::submit(DecodedFile file) {
  impl_->submit(std::move(file));
}
void ExtractionWriter::finish() {
  impl_->finish();
}
} // namespace slpz
