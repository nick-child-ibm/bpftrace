#include "bpfbytecode.h"

#include <bpf/btf.h>
#include <stdexcept>

namespace bpftrace {

void BpfBytecode::addSection(const std::string &name,
                             std::vector<uint8_t> &&data)
{
  sections_.emplace(name, data);
}

bool BpfBytecode::hasSection(const std::string &name) const
{
  return sections_.find(name) != sections_.end();
}

const std::vector<uint8_t> &BpfBytecode::getSection(
    const std::string &name) const
{
  if (!hasSection(name))
  {
    throw std::runtime_error("Bytecode is missing section " + name);
  }
  return sections_.at(name);
}

// This is taken from libbpf (btf.c) and we need it to manually iterate BTF
// entries so that we can fix them up in place.
// Should go away once we let libbpf do the BTF fixup.
static int btf_type_size(const struct btf_type *t)
{
  const int base_size = sizeof(struct btf_type);
  __u16 vlen = btf_vlen(t);

  switch (btf_kind(t))
  {
    case BTF_KIND_FWD:
    case BTF_KIND_CONST:
    case BTF_KIND_VOLATILE:
    case BTF_KIND_RESTRICT:
    case BTF_KIND_PTR:
    case BTF_KIND_TYPEDEF:
    case BTF_KIND_FUNC:
    case BTF_KIND_FLOAT:
    case BTF_KIND_TYPE_TAG:
      return base_size;
    case BTF_KIND_INT:
      return base_size + sizeof(__u32);
    case BTF_KIND_ENUM:
      return base_size + vlen * sizeof(struct btf_enum);
    case BTF_KIND_ENUM64:
      return base_size + vlen * sizeof(struct btf_enum64);
    case BTF_KIND_ARRAY:
      return base_size + sizeof(struct btf_array);
    case BTF_KIND_STRUCT:
    case BTF_KIND_UNION:
      return base_size + vlen * sizeof(struct btf_member);
    case BTF_KIND_FUNC_PROTO:
      return base_size + vlen * sizeof(struct btf_param);
    case BTF_KIND_VAR:
      return base_size + sizeof(struct btf_var);
    case BTF_KIND_DATASEC:
      return base_size + vlen * sizeof(struct btf_var_secinfo);
    case BTF_KIND_DECL_TAG:
      return base_size + sizeof(struct btf_decl_tag);
    default:
      return -EINVAL;
  }
}

// There are cases when BTF generated by LLVM needs to be patched. This is
// normally done by libbpf but only when loading via bpf_object is used. We're
// currently using direct loading using bpf_btf_load and bpf_prog_load so we
// need to mimic libbpf's behavior and do the patching manually.
// This should go away once we move to bpf_object-based loading.
//
// Transformations done:
// - If running on a kernel not supporting BTF func entries with BTF_FUNC_GLOBAL
//   linkage, change the linkage type to 0. We cannot do this directly in
//   codegen b/c LLVM would optimize our functions away.
void BpfBytecode::fixupBTF(BPFfeature &feature)
{
  if (!hasSection(".BTF"))
    return;

  auto &btfsec = sections_[".BTF"];
  auto *btfhdr = reinterpret_cast<struct btf_header *>(btfsec.data());
  auto *btf = btf__new(btfsec.data(), btfsec.size());

  auto *types_start = btfsec.data() + sizeof(struct btf_header) +
                      btfhdr->type_off;
  auto *types_end = types_start + btfhdr->type_len;

  // Unfortunately, libbpf's btf__type_by_id returns a const pointer which
  // doesn't allow modification. So, we must iterate the types manually.
  auto *ptr = types_start;
  while (ptr + sizeof(struct btf_type) <= types_end)
  {
    auto *btf_type = reinterpret_cast<struct btf_type *>(ptr);
    ptr += btf_type_size(btf_type);

    // Change linkage type to 0 if the kernel doesn't support BTF_FUNC_GLOBAL.
    if (!feature.has_btf_func_global() &&
        BTF_INFO_KIND(btf_type->info) == BTF_KIND_FUNC)
    {
      btf_type->info = BTF_INFO_ENC(BTF_KIND_FUNC, 0, 0);
    }
  }
  btf__free(btf);
}

} // namespace bpftrace
