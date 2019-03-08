#include <string.h>

#include <map>
#include <string>
#include <tuple>
#include <fstream>
#include <memory>

#include "utils.h"
#include "bcc_usdt.h"

namespace {

std::vector<int> read_cpu_range(std::string path)
{
  std::ifstream cpus_range_stream { path };
  std::vector<int> cpus;
  std::string cpu_range;

  while (std::getline(cpus_range_stream, cpu_range, ',')) {
    std::size_t rangeop = cpu_range.find('-');
    if (rangeop == std::string::npos) {
      cpus.push_back(std::stoi(cpu_range));
    }
    else {
      int start = std::stoi(cpu_range.substr(0, rangeop));
      int end = std::stoi(cpu_range.substr(rangeop + 1));
      for (int i = start; i <= end; i++)
        cpus.push_back(i);
    }
  }
  return cpus;
}

} // namespace

namespace bpftrace {

static bool usdt_probe_cached = false;
static std::map<std::string, usdt_probe_pair> usdt_probe_cache_outer;

static void usdt_probe_each(struct bcc_usdt *usdt_probe) {
  usdt_probe_cache_outer[usdt_probe->name] = std::make_tuple(usdt_probe->provider, usdt_probe->bin_path);
}

usdt_probe_pair USDTHelper::find(void *ctx, int pid, std::string name) {
  bool ctx_created = false;

  if (!usdt_probe_cached) {
    if (ctx == nullptr) {
      ctx_created = true;
      ctx = bcc_usdt_new_frompid(pid, nullptr);
      if (ctx == nullptr)
        return std::make_tuple("", "");
    }

    bcc_usdt_foreach(ctx, usdt_probe_each);
    usdt_probe_cached = true;

    if (ctx_created)
      bcc_usdt_close(ctx);
  }

  std::map<std::string, usdt_probe_pair>::iterator p = usdt_probe_cache_outer.find(name);
  if (p == usdt_probe_cache_outer.end())
    return std::make_tuple("", "");
  else
    return p->second;
}

bool has_wildcard(const std::string &str)
{
  if (str.find("*") != std::string::npos ||
     (str.find("[") != std::string::npos &&
      str.find("]") != std::string::npos))
     return true;
  else
     return false;
}

std::vector<int> get_online_cpus()
{
  return read_cpu_range("/sys/devices/system/cpu/online");
}

std::vector<int> get_possible_cpus()
{
  return read_cpu_range("/sys/devices/system/cpu/possible");
}

std::vector<std::string> get_kernel_cflags(
    const char* uname_machine,
    const std::string& ksrc,
    const std::string& kobj)
{
  std::vector<std::string> cflags;
  std::string arch = uname_machine;
  const char *archenv;

  if (!strncmp(uname_machine, "x86_64", 6)) {
    arch = "x86";
  } else if (uname_machine[0] == 'i' && !strncmp(&uname_machine[2], "86", 2)) {
    arch = "x86";
  } else if (!strncmp(uname_machine, "arm", 3)) {
    arch = "arm";
  } else if (!strncmp(uname_machine, "sa110", 5)) {
    arch = "arm";
  } else if (!strncmp(uname_machine, "s390x", 5)) {
    arch = "s390";
  } else if (!strncmp(uname_machine, "parisc64", 8)) {
    arch = "parisc";
  } else if (!strncmp(uname_machine, "ppc", 3)) {
    arch = "powerpc";
  } else if (!strncmp(uname_machine, "mips", 4)) {
    arch = "mips";
  } else if (!strncmp(uname_machine, "sh", 2)) {
    arch = "sh";
  } else if (!strncmp(uname_machine, "aarch64", 7)) {
    arch = "arm64";
  }

  // If ARCH env is defined, use it over uname
  archenv = getenv("ARCH");
  if (archenv)
    arch = std::string(archenv);

  cflags.push_back("-nostdinc");
  cflags.push_back("-isystem");
  cflags.push_back("/virtual/lib/clang/include");

  // see linux/Makefile for $(LINUXINCLUDE) + $(USERINCLUDE)
  cflags.push_back("-I" + ksrc + "/arch/"+arch+"/include");
  cflags.push_back("-I" + kobj + "/arch/"+arch+"/include/generated");
  cflags.push_back("-I" + ksrc + "/include");
  cflags.push_back("-I" + kobj + "/include");
  cflags.push_back("-I" + ksrc + "/arch/"+arch+"/include/uapi");
  cflags.push_back("-I" + kobj + "/arch/"+arch+"/include/generated/uapi");
  cflags.push_back("-I" + ksrc + "/include/uapi");
  cflags.push_back("-I" + kobj + "/include/generated/uapi");

  cflags.push_back("-include");
  cflags.push_back(ksrc + "/include/linux/kconfig.h");
  cflags.push_back("-D__KERNEL__");
  cflags.push_back("-D__BPF_TRACING__");
  cflags.push_back("-D__HAVE_BUILTIN_BSWAP16__");
  cflags.push_back("-D__HAVE_BUILTIN_BSWAP32__");
  cflags.push_back("-D__HAVE_BUILTIN_BSWAP64__");

  // If ARCH env variable is set, pass this along.
  if (archenv)
	cflags.push_back("-D__TARGET_ARCH_" + arch);

  return cflags;
}

std::string is_deprecated(std::string &str)
{

  std::vector<DeprecatedName>::iterator item;

  for (item = DEPRECATED_LIST.begin(); item != DEPRECATED_LIST.end(); item++)
  {
    if (str.compare(item->old_name) == 0)
    {
      if (item->show_warning)
      {
        std::cerr << "warning: " << item->old_name << " is deprecated and will be removed in the future. ";
        std::cerr << "Use " << item->new_name << " instead." << std::endl;
        item->show_warning = false;
      }

      return item->new_name;
    }
  }

  return str;
}

std::string exec_system(const char* cmd)
{
  std::array<char, 128> buffer;
  std::string result;
  std::shared_ptr<FILE> pipe(popen(cmd, "r"), pclose);
  if (!pipe) throw std::runtime_error("popen() failed!");
  while (!feof(pipe.get())) {
    if (fgets(buffer.data(), 128, pipe.get()) != nullptr)
      result += buffer.data();
  }
  return result;
}

std::string resolve_binary_path(const std::string& cmd)
{
  std::string query;
  query += "command -v ";
  query += cmd;
  std::string result = exec_system(query.c_str());

  if (result.size())
  {
    // Remove newline at the end
    auto it = result.rfind('\n');
    if (it != std::string::npos)
      result.erase(it);

    return result;
  }
  else
  {
    return cmd;
  }
}

} // namespace bpftrace
