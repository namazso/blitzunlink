//  blitzunlink - A Blitz blob to COFF converter.
//  Copyright (C) 2022  namazso
//  
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//  
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//  
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <https://www.gnu.org/licenses/>.
#include <Windows.h>
#include <fstream>
#include <unordered_map>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <map>
#include <set>
#include <coffi/coffi.hpp>

struct ProcessedBB
{
  std::vector<uint8_t> content;
  std::vector<std::pair<uint32_t, std::string>> exports;
  std::vector<std::pair<uint32_t, std::string>> imports_relative;
  std::vector<std::pair<uint32_t, std::string>> imports_absolute;
};

inline bool process_bb(void* p, ProcessedBB& processed)
{
  struct Sym
  {
    std::string name;
    uint32_t rva{};
  };

  const static auto get_sym = [](void** p)
  {
    Sym sym;
    auto t = (char*)*p;
    while (const char c = *t++)
      sym.name += c;
    sym.rva = *(uint32_t*)t;
    *p = t + 4;
    return sym;
  };

  processed = {};

  const auto sz = *(uint32_t*)p;
  p = (uint32_t*)p + 1;

  const auto module_base = p;

  p = (char*)p + sz;

  // exports
  {
    const auto cnt = *(uint32_t*)p;
    p = (uint32_t*)p + 1;
    for (uint32_t k = 0; k < cnt; ++k)
    {
      Sym sym = get_sym(&p);
      if (sym.rva >= sz)
        return false;
      processed.exports.emplace_back(sym.rva, sym.name);
    }
  }

  // relative imports
  {
    const auto cnt = *(uint32_t*)p;
    p = (uint32_t*)p + 1;
    for (uint32_t k = 0; k < cnt; ++k)
    {
      Sym sym = get_sym(&p);
      processed.imports_relative.emplace_back(sym.rva, sym.name);
    }
  }

  // absolute imports
  {
    const auto cnt = *(uint32_t*)p;
    p = (uint32_t*)p + 1;
    for (uint32_t k = 0; k < cnt; ++k)
    {
      Sym sym = get_sym(&p);
      processed.imports_absolute.emplace_back(sym.rva, sym.name);
    }
  }

  std::sort(begin(processed.exports), end(processed.exports));
  std::sort(begin(processed.imports_relative), end(processed.imports_relative));
  std::sort(begin(processed.imports_absolute), end(processed.imports_absolute));

  // copy the section contents at last
  processed.content = {(uint8_t*)module_base, (uint8_t*)module_base + sz};

  return true;
}

static std::vector<uint8_t> read_all(const char* path)
{
  std::ifstream is(path, std::ios::binary);
  if (is.bad() || !is.is_open())
    return {};
  is.seekg(0, std::ifstream::end);
  std::vector<uint8_t> data;
  data.resize((size_t)is.tellg());
  is.seekg(0, std::ifstream::beg);
  is.read(reinterpret_cast<char*>(data.data()), (std::streamsize)data.size());
  return data;
}

void write_all(const char* path, const void* data, size_t size)
{
  std::ofstream os(path, std::ios::binary);
  os.write((const char*)data, size);
  os.close();
}

int main(int argc, char** argv)
{
  if (argc != 3)
  {
    fprintf(stderr, "Usage: %s <input.bin> <output.obj>\n", argv[0]);
    return 1;
  }

  ProcessedBB processed{};

  {
    auto bbfile = read_all(argv[1]);
    if (bbfile.size() < 4)
    {
      fprintf(stderr, "Failed to read file\n");
      return 2;
    }

    if (!process_bb(bbfile.data(), processed))
    {
      fprintf(stderr, "Failed to process bb file\n");
      return 3;
    }
  }

  // Structure / sections of BB module contents:
  // 1. Functions. Every function and label has an export. Starts at 0, also exported as "__MAIN"
  // 2. RW Data. Identifiable by finding the lowest RVA export that is imported absolutely.
  // 3. Libs. A packed string array of dlls to load. Considered .rdata. Starts at export "__LIBS"
  // 4. RO Data. Various structure descrptions that are only read during initialization. Starts at export "__DATA"

  // we cheat a bit
  const auto cheat_imported_enum = [&](const char* name, uint32_t value)
  {
    const auto old_size = processed.content.size();
    processed.content.insert(processed.content.end(), (uint8_t*)&value, (uint8_t*)(&value + 1));
    processed.exports.emplace_back(old_size, name);
  };

  // these never changed in blitz
  cheat_imported_enum("__bbIntType", 1);
  cheat_imported_enum("__bbFltType", 2);
  cheat_imported_enum("__bbStrType", 3);
  cheat_imported_enum("__bbCStrType", 4);
  // haven't seen these but these are the enum values
  cheat_imported_enum("__bbObjType", 5);
  cheat_imported_enum("__bbVecType", 6);

  // fixup for COFF rel32 being rip-relative
  for (const auto& imp : processed.imports_relative)
    *(uint32_t*)&processed.content[imp.first] += 4;

  const auto find_sym = +[](std::vector<std::pair<uint32_t, std::string>>& vec, const std::string& name)
  {
    return std::find_if(
      begin(vec),
      end(vec),
      [&](const auto& v)
      {
        return v.second == name;
      }
    );
  };

  uint32_t rva_data = 0xFFFFFFFF;

  for (const auto& imp : processed.imports_absolute)
  {
    if (0 == imp.second.compare(0, 2, "_f"))
      continue; // absolute importing a function

    const auto resolved = find_sym(processed.exports, imp.second);
    if (resolved == processed.exports.end())
      continue; // external import

    if (resolved->first < rva_data)
      rva_data = resolved->first;
  }

  if (rva_data >= processed.content.size())
  {
    fprintf(stderr, "Cannot guess .data base\n");
    return 5;
  }

  printf("Guessed .data base: %08X\n", rva_data);

  uint32_t rva_rdata = 0xFFFFFFFF;

  const auto exp_data = find_sym(processed.exports, "__DATA");
  if (exp_data != processed.exports.end())
    rva_rdata = (std::min)(exp_data->first, rva_rdata);

  const auto exp_lib = find_sym(processed.exports, "__LIBS");
  if (exp_lib != processed.exports.end())
    rva_rdata = (std::min)(exp_lib->first, rva_rdata);

  const auto exp_cstrs = find_sym(processed.exports, "__CSTRS");
  if (exp_cstrs != processed.exports.end())
    rva_rdata = (std::min)(exp_cstrs->first, rva_rdata);

  std::set<std::string> userlib_imports;
  if (exp_lib != processed.exports.end() && exp_data != processed.exports.end() && exp_lib->first < exp_data->first)
    for (const auto& imp : processed.imports_absolute)
      if (imp.first >= exp_lib->first && imp.first < exp_data->first)
        userlib_imports.emplace(imp.second);

  if (rva_rdata == 0xFFFFFFFF)
  {
    fprintf(stderr, "Error: no __DATA or __LIBS export\n");
    return 7;
  }
  
  using namespace COFFI;

  coffi writer;
  
  writer.create(COFFI_ARCHITECTURE_PE);

  writer.get_header()->set_flags(IMAGE_FILE_32BIT_MACHINE | IMAGE_FILE_LINE_NUMS_STRIPPED);

  std::map<std::string, uint32_t> sym_indices;

  // .text
  const auto text_sec = writer.add_section(".text");
  {
    text_sec->set_flags(IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_CODE | IMAGE_SCN_ALIGN_1BYTES);

    text_sec->set_data((const char*)processed.content.data(), rva_data);

    {
      const auto old_size = text_sec->get_data_size();
      text_sec->append_data("\xb8\x01\x00\x00\x00\xc2\x0c\x00", 8);

      const auto sym_entry = writer.add_symbol("_DllEntry");
      sym_entry->set_type(IMAGE_SYM_TYPE_FUNCTION);
      sym_entry->set_storage_class(IMAGE_SYM_CLASS_EXTERNAL);
      sym_entry->set_section_number(text_sec->get_index() + 1);
      sym_entry->set_value(old_size);
      sym_entry->set_aux_symbols_number(1);
      auxiliary_symbol_record aux{};
      sym_entry->get_auxiliary_symbols().push_back(aux);
    }

    for (const auto& exp : processed.exports)
    {
      if (exp.first >= rva_data)
        continue;

      const auto sym = writer.add_symbol("_" + exp.second);

      if (0 == exp.second.compare(0, 2, "_f") || exp.second == "__MAIN") // function
      {
        sym->set_type(IMAGE_SYM_TYPE_FUNCTION);
        sym->set_storage_class(IMAGE_SYM_CLASS_EXTERNAL);
      }
      else if (0 == exp.second.compare(0, 3, "_l_") // label
        || (exp.second.size() > 1 && exp.second[0] == '_' && isdigit(exp.second[1])))
      {
        sym->set_type(IMAGE_SYM_TYPE_FUNCTION);
        sym->set_storage_class(IMAGE_SYM_CLASS_STATIC);
      }
      else
      {
        fprintf(stderr, "Error: unknown type .text symbol %s\n", exp.second.c_str());
        return 8;
      }
      sym->set_section_number(text_sec->get_index() + 1);
      sym->set_value(exp.first);
      sym->set_aux_symbols_number(1);
      auxiliary_symbol_record aux{};
      sym->get_auxiliary_symbols().push_back(aux);

      sym_indices[exp.second] = sym->get_index();
    }
  }

  // .data
  const auto data_sec = writer.add_section(".data");
  {
    data_sec->set_flags(IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_ALIGN_1BYTES);

    data_sec->set_data((const char*)processed.content.data() + rva_data, rva_rdata - rva_data);

    for (const auto& exp : processed.exports)
    {
      if (exp.first < rva_data || exp.first >= rva_rdata)
        continue;

      const auto sym = writer.add_symbol("_" + exp.second);

      if (0 == exp.second.compare(0, 2, "_a")
        || (userlib_imports.find(exp.second) != userlib_imports.end()) // userlib stuff
        || 0 == exp.second.compare(0, 2, "_v")
        || 0 == exp.second.compare(0, 2, "_t")
        || (exp.second.size() > 1 && exp.second[0] == '_' && isdigit(exp.second[1]))) // data
      {
        sym->set_type(IMAGE_SYM_TYPE_NOT_FUNCTION);
        sym->set_storage_class(IMAGE_SYM_CLASS_EXTERNAL);
      }
      else
      {
        fprintf(stderr, "Error: unknown type .data symbol %s\n", exp.second.c_str());
        return 9;
      }
      sym->set_section_number(data_sec->get_index() + 1);
      sym->set_value(exp.first - rva_data);
      sym->set_aux_symbols_number(1);
      auxiliary_symbol_record aux{};
      sym->get_auxiliary_symbols().push_back(aux);

      sym_indices[exp.second] = sym->get_index();
    }
  }
  // .rdata
  const auto rdata_sec = writer.add_section(".rdata");
  {
    rdata_sec->set_flags(IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_ALIGN_1BYTES);

    rdata_sec->set_data((const char*)processed.content.data() + rva_rdata, processed.content.size() - rva_rdata);

    for (const auto& exp : processed.exports)
    {
      if (exp.first < rva_rdata || exp.first >= processed.content.size())
        continue;

      const auto sym = writer.add_symbol("_" + exp.second);

      if (exp.second == "__DATA"
        || exp.second == "__LIBS" 
        || exp.second == "__CSTRS" 
        || 0 == exp.second.compare(0, 4, "__bb"))
      {
        sym->set_type(IMAGE_SYM_TYPE_NOT_FUNCTION);
        sym->set_storage_class(IMAGE_SYM_CLASS_EXTERNAL);
      }
      else
      {
        fprintf(stderr, "Error: unknown type .rdata symbol %s\n", exp.second.c_str());
        return 10;
      }
      sym->set_section_number(rdata_sec->get_index() + 1);
      sym->set_value(exp.first - rva_rdata);
      sym->set_aux_symbols_number(1);
      auxiliary_symbol_record aux{};
      sym->get_auxiliary_symbols().push_back(aux);

      sym_indices[exp.second] = sym->get_index();
    }
  }

  for (const auto& imp : processed.imports_relative)
  {
    if (sym_indices.find(imp.second) != sym_indices.end())
      continue;
    const auto sym = writer.add_symbol("_" + imp.second);
    sym->set_type(IMAGE_SYM_TYPE_FUNCTION);
    sym->set_storage_class(IMAGE_SYM_CLASS_EXTERNAL);
    sym->set_section_number(IMAGE_SYM_UNDEFINED);
    sym->set_aux_symbols_number(1);
    auxiliary_symbol_record aux{};
    sym->get_auxiliary_symbols().push_back(aux);
    sym_indices[imp.second] = sym->get_index();
  }

  const auto rva_to_section_offset = [&](uint32_t rva) -> std::pair<section*, uint32_t>
  {
    if (rva < rva_data)
      return {text_sec, rva};
    if (rva < rva_rdata)
      return {data_sec, rva - rva_data};
    if (rva < processed.content.size())
      return {rdata_sec, rva - rva_rdata};
    return {};
  };

  for (const auto& imp : processed.imports_relative)
  {
    const auto section_offset = rva_to_section_offset(imp.first);
    const auto symbol_idx = sym_indices.at(imp.second);
    rel_entry rel
    {
      section_offset.second,
      symbol_idx,
      IMAGE_REL_I386_REL32
    };
    section_offset.first->add_relocation_entry((const rel_entry_generic*)&rel);
  }

  for (const auto& imp : processed.imports_absolute)
  {
    const auto section_offset = rva_to_section_offset(imp.first);
    const auto symbol_idx = sym_indices.find(imp.second);
    if (symbol_idx == sym_indices.end())
    {
      fprintf(stderr, "Error: absolute importing external symbol %s\n", imp.second.c_str());
      return 11;
    }
    rel_entry rel
    {
      section_offset.second,
      symbol_idx->second,
      IMAGE_REL_I386_DIR32
    };
    section_offset.first->add_relocation_entry((const rel_entry_generic*)&rel);
  }

  const auto add_section_symbol = [&](const section* sec)
  {
    symbol* sym = writer.add_symbol(sec->get_name());
    sym->set_type(IMAGE_SYM_TYPE_NOT_FUNCTION);
    sym->set_storage_class(IMAGE_SYM_CLASS_STATIC);
    sym->set_section_number(sec->get_index() + 1);
    sym->set_aux_symbols_number(1);
    auxiliary_symbol_record_5 aux{
      sec->get_data_size(), (uint16_t)sec->get_reloc_count(), 0, 0, 0, 0, {0, 0, 0}
    };
    sym->get_auxiliary_symbols().push_back(*reinterpret_cast<auxiliary_symbol_record*>(&aux));
  };

  add_section_symbol(text_sec);
  add_section_symbol(data_sec);
  add_section_symbol(rdata_sec);

  writer.save(argv[2]);

  return 0;
}
