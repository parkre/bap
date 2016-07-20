#ifndef LLVM_BINARY_HPP
#define LLVM_BINARY_HPP

#include <memory>
#include <numeric>
#include <vector>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <sstream>

#include <llvm/ADT/iterator_range.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/COFF.h>
#include <llvm/Object/MachO.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/SymbolSize.h>
#include <llvm/Support/Compiler.h>
#include <llvm/Config/llvm-config.h>

using std::move;

#if LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR == 8
#include <llvm/ADT/iterator_range.h>
#include <llvm/Object/SymbolSize.h>
#endif

#if LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR == 4
#include <llvm/ADT/OwningPtr.h>
#endif

#if LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR == 8
using std::error_code;
#endif

/*
#if LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR == 4
using llvm::error_code;
#endif
*/

extern "C" void llvm_binary_fail(const char*) LLVM_ATTRIBUTE_NORETURN ;

LLVM_ATTRIBUTE_NORETURN void llvm_binary_fail (const error_code ec) {
    llvm_binary_fail(ec.message().c_str());
}

namespace llvm { namespace object {

template <typename T>
content_iterator<T>& operator++(content_iterator<T>& a) {
    error_code ec;
    a.increment(ec);
    if(ec) llvm_binary_fail(ec);
    return a;
}

}} //namespace llvm::object

namespace utils {
using namespace llvm;
using namespace llvm::object;

const pe32plus_header* getPE32PlusHeader(const COFFObjectFile& obj) {
    uint64_t cur_ptr = 0;
    const char *buf = (obj.getData()).data();
    const uint8_t *start = reinterpret_cast<const uint8_t *>(buf);
    uint8_t b0 = start[0];
    uint8_t b1 = start[1];
    if (b0 == 0x4d && b1 == 0x5a) { // check if this is a PE/COFF file
        // a pointer at offset 0x3C points to the
        cur_ptr += *reinterpret_cast<const uint16_t *>(start + 0x3c);
        // check the PE magic bytes.
        if (std::memcmp(start + cur_ptr, "PE\0\0", 4) != 0)
            llvm_binary_fail("PE Plus header not found");
        cur_ptr += 4; // skip the PE magic bytes.
        cur_ptr += sizeof(coff_file_header);
        return reinterpret_cast<const pe32plus_header *>(start + cur_ptr);
    }
    return NULL;
}

} //namespace utils

namespace {

template<typename Derived, typename Base>
std::unique_ptr<Derived> dynamic_unique_ptr_cast(std::unique_ptr<Base>&& ptr) {
    if (Derived* d = llvm::dyn_cast<Derived>(ptr.get())) {
	ptr.release();
	return std::unique_ptr<Derived>(d);
    }
    return std::unique_ptr<Derived>(nullptr);
}

} // namespace

namespace seg {
using namespace llvm;
using namespace llvm::object;

struct segment {
    std::string name;
    uint64_t offset;
    uint64_t addr;
    uint64_t size;
    bool is_readable;
    bool is_writable;
    bool is_executable;
};


template<typename T>
std::vector<segment> read(const ELFObjectFile<T>& obj) {
    auto begin = obj.getELFFile()->program_header_begin();
    auto end = obj.getELFFile()->program_header_end();
    std::vector<segment> segments;
    segments.reserve(std::distance(begin, end));
    auto it = begin;
    for (int pos = 0; it != end; ++it, ++pos) {
        if (it -> p_type == ELF::PT_LOAD) {
	    std::ostringstream oss;
	    oss << std::setfill('0') << std::setw(2) << pos;
	    segments.push_back(segment{oss.str(),
			it->p_offset,
			it->p_vaddr,
			it->p_filesz,
			static_cast<bool>(it->p_flags & ELF::PF_R),
			static_cast<bool>(it->p_flags & ELF::PF_W),
			static_cast<bool>(it->p_flags & ELF::PF_X)});
	}
    }
    return segments;
}

template <typename S>
segment make_segment(const S &s) {
    return segment{s.segname, s.fileoff, s.vmaddr, s.filesize,
	    static_cast<bool>(s.initprot & MachO::VM_PROT_READ),
	    static_cast<bool>(s.initprot & MachO::VM_PROT_WRITE),
	    static_cast<bool>(s.initprot & MachO::VM_PROT_EXECUTE)};
}

std::vector<segment> read(const MachOObjectFile& obj) {
    auto cmds = obj.load_commands();
    std::vector<segment> segments;
    for (auto it : cmds) {
        if (it.C.cmd == MachO::LoadCommandType::LC_SEGMENT_64)
            segments.push_back(make_segment(obj.getSegment64LoadCommand(it)));
        if (it.C.cmd == MachO::LoadCommandType::LC_SEGMENT)
            segments.push_back(make_segment(obj.getSegmentLoadCommand(it)));
    }
    return segments;
}

template <typename T>
segment make_segment(T image_base, const coff_section &s) {
    return segment{s.Name,
	    static_cast<T>(s.PointerToRawData),
	    static_cast<T>(s.VirtualAddress + image_base),
	    static_cast<T>(s.SizeOfRawData),
	    static_cast<bool>((s.Characteristics) &
			      COFF::IMAGE_SCN_MEM_READ),
	    static_cast<bool>((s.Characteristics) &
			      COFF::IMAGE_SCN_MEM_WRITE),
	    static_cast<bool>((s.Characteristics) &
			      COFF::IMAGE_SCN_MEM_EXECUTE)};
}

template <typename T>
std::vector<segment> readPE(const COFFObjectFile& obj, const T image_base) {
    std::vector<segment> segments;
    for (auto it : obj.sections()) {
        const coff_section *s = obj.getCOFFSection(it);
        T c = static_cast<T>(s->Characteristics);
        if ( c & COFF::IMAGE_SCN_CNT_CODE ||
             c & COFF::IMAGE_SCN_CNT_INITIALIZED_DATA ||
             c & COFF::IMAGE_SCN_CNT_UNINITIALIZED_DATA )
            segments.push_back(make_segment<T>(image_base, *s));
    }
    return segments;
}

std::vector<segment> read(const COFFObjectFile& obj) {
    if (obj.getBytesInAddress() == 4) {
	const pe32_header *pe32;
	if (error_code ec = obj.getPE32Header(pe32))
	    llvm_binary_fail(ec);
	return readPE<uint32_t>(obj, pe32->ImageBase);
    } else {
        const pe32plus_header *pe32plus;
        #if LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR == 8
        if (error_code ec = obj.getPE32PlusHeader(pe32plus))
            llvm_binary_fail(ec);
        #else
        pe32plus = utils::getPE32PlusHeader(obj);
        if (!pe32plus)
            llvm_binary_fail("Failed to extract PE32+ header");
        #endif
        return readPE<uint64_t>(obj, pe32plus->ImageBase);
    }
}

} //namespace seg

namespace sym {
using namespace llvm;
using namespace llvm::object;

typedef SymbolRef::Type kind_type;

struct symbol {
    std::string name;
    kind_type kind;
    uint64_t addr;
    uint64_t size;
};

#if LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR == 8
symbol make_symbol(const SymbolRef& sym, uint64_t size) {
    auto name = sym.getName();
    if (error_code ec = name.getError())
        llvm_binary_fail(ec);

    auto addr = sym.getAddress();
    if (error_code ec = addr.getError())
        llvm_binary_fail(ec);

    return symbol{name->str(), sym.getType(), addr.get(), size};
}

std::vector<symbol> read(const ObjectFile& obj) {
    std::vector<symbol> symbols;
    auto symbol_sizes = computeSymbolSizes(obj);
    for (auto s : symbol_sizes)
	symbols.push_back(make_symbol(s.first, s.second));
    return symbols;
}
#endif

#if LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR == 4
symbol make_symbol(const SymbolRef &sym) {
    StringRef name;
    if(error_code err = sym.getName(name))
	llvm_binary_fail(err);
    
    kind_type kind;
    if (error_code err = sym.getType(kind))
	llvm_binary_fail(err);

    uint64_t addr;
    if (error_code err = sym.getAddress(addr))
	llvm_binary_fail(err);

    uint64_t size;
    if (error_code err = sym.getSize(size))
	llvm_binary_fail(err);
    return symbol{name.str(), kind, addr, size};
}

template <typename OutputIterator>
OutputIterator read(symbol_iterator begin, 
		    symbol_iterator end,
		    OutputIterator out) {
    return std::transform(begin, end, out,
			  [](const SymbolRef& s) { return make_symbol(s); });
}

std::vector<symbol> read(const ObjectFile& obj) {
    int size = utils::distance(obj.begin_symbols(),
                               obj.end_symbols());
    std::vector<symbol> symbols;
    symbols.reserve(size);

    read(obj.begin_symbols(),
         obj.end_symbols(),
         std::back_inserter(symbols));
    return symbols;
}

symbol make_symbol(const SymbolRef& sym, uint64_t addr, uint64_t size) {
    StringRef name;
    if(error_code err = sym.getName(name))
	llvm_binary_fail(err);

    kind_type kind;
    if (error_code err = sym.getType(kind))
	llvm_binary_fail(err);
    return symbol{name.str(), kind, addr, size};
}

std::vector<symbol> read(const COFFObjectFile& obj, const uint64_t image_base) {
    std::vector<symbol> symbols;
    for (auto it = obj.begin_symbols(); it != obj.end_symbols(); ++it) {
        auto sym = obj.getCOFFSymbol(it);

        if (!sym) llvm_binary_fail("not a coff symbol");

        const coff_section *sec = nullptr;
        if (sym->SectionNumber == COFF::IMAGE_SYM_UNDEFINED)
            continue;

        if (error_code ec = obj.getSection(sym->SectionNumber, sec))
            llvm_binary_fail(ec);

        if (!sec) continue;

        uint64_t size = (sec->VirtualAddress + sec->SizeOfRawData) - sym->Value;

        for (auto it = obj.begin_symbols(); it != obj.end_symbols(); ++it) {
            auto next = obj.getCOFFSymbol(it);
            if (next->SectionNumber == sym->SectionNumber) {
                auto new_size = next->Value > sym->Value ?
                    next->Value - sym->Value : size;
                size = new_size < size ? new_size : size;
            }
        }

        auto addr = sec->VirtualAddress + image_base + sym->Value;
        symbols.push_back(make_symbol(*it,addr,size));
    }
    return symbols;
}

std::vector<symbol> read(const COFFObjectFile& obj) {
    if (obj.getBytesInAddress() == 4) {
	const pe32_header *pe32;
	if (error_code err = obj.getPE32Header(pe32))
	    llvm_binary_fail(err);
	return read(obj, pe32->ImageBase);
    } else {
	const pe32plus_header *pe32plus = utils::getPE32PlusHeader(obj);
	if (!pe32plus)
	    llvm_binary_fail("Failed to extract PE32+ header");
	return read(obj, pe32plus->ImageBase);
    }
}

template <typename ELFT>
std::vector<symbol> read(const ELFObjectFile<ELFT>& obj) {
    int size1 = utils::distance(obj.begin_symbols(),
                                obj.end_symbols());
    int size2 = utils::distance(obj.begin_dynamic_symbols(),
                                obj.end_dynamic_symbols());

    std::vector<symbol> symbols;
    symbols.reserve(size1+size2);

    auto it = read(obj.begin_symbols(),
                   obj.end_symbols(),
                   std::back_inserter(symbols));

    read(obj.begin_dynamic_symbols(),
         obj.end_dynamic_symbols(),
         it);
    return symbols;
}
#endif

} //namespace sym

namespace sec {
using namespace llvm;
using namespace llvm::object;

struct section {
    std::string name;
    uint64_t addr;
    uint64_t size;
};

section make_section(const SectionRef &sec) {
    StringRef name;
    if (error_code ec = sec.getName(name))
	llvm_binary_fail(ec);

    #if LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR == 4
    uint64_t addr;
    if (error_code err = sec.getAddress(addr))
	llvm_binary_fail(err);

    uint64_t size;
    if (error_code err = sec.getSize(size))
	llvm_binary_fail(err);
    return section{name.str(), addr, size};
    #else
    return section{name.str(), sec.getAddress(), sec.getSize()};
    #endif
}

std::vector<section> read(const ObjectFile &obj) {
    auto size = std::distance(obj.sections().begin(),
                              obj.sections().end());
    std::vector<section> sections;
    sections.reserve(size);
    std::transform(obj.sections().begin(),
                   obj.sections().end(),
                   std::back_inserter(sections),
                   [](const SectionRef& s) { return make_section(s); });
    return sections;
}

} //namespace sec

namespace img {
using namespace llvm;
using namespace llvm::object;

struct image {
    virtual uint64_t entry() const = 0;
    virtual std::string arch() const = 0;
    virtual const std::vector<seg::segment>& segments() const = 0;
    virtual const std::vector<sym::symbol>& symbols() const = 0;
    virtual const std::vector<sec::section>& sections() const = 0;
    virtual ~image() {}
};

std::string image_arch(const ObjectFile& obj) {
    return llvm::Triple::getArchTypeName(static_cast<Triple::ArchType>(obj.getArch()));
}

template <typename ELFT>
uint64_t image_entry(const ELFObjectFile<ELFT>& obj) {
    return obj.getELFFile()->getHeader()->e_entry;
}

uint64_t image_entry(const MachOObjectFile& obj) {
    typedef MachOObjectFile::LoadCommandInfo command_info;
    //typedef std::vector<command_info> commands;
    typedef std::vector<command_info>::const_iterator const_iterator;
    MachOObjectFile::load_command_iterator it =
        std::find_if(obj.begin_load_commands(), obj.end_load_commands(),
                     [](const command_info &info){
                         return
                         info.C.cmd == MachO::LoadCommandType::LC_MAIN;});
    if (it != obj.end_load_commands()) {
        const MachO::entry_point_command *entry_cmd =
            reinterpret_cast<const MachO::entry_point_command*>(it->Ptr);
        return entry_cmd->entryoff;
    } else {
        llvm_binary_fail("LC_MAIN not found, binary version < 10.8");
    }
}

uint64_t image_entry(const COFFObjectFile& obj) {
    if (obj.getBytesInAddress() == 4) {
        const pe32_header* hdr = 0;
        if (error_code ec = obj.getPE32Header(hdr))
	    llvm_binary_fail(ec);
        if (!hdr)
            llvm_binary_fail("PE header not found");
        return hdr->AddressOfEntryPoint;
    } else {
	const pe32plus_header* hdr = 0;
	if (error_code ec = obj.getPE32PlusHeader(hdr))
	    llvm_binary_fail(ec);
	if (!hdr)
	    llvm_binary_fail("PEplus header no found");
	return hdr->AddressOfEntryPoint;
    }
}

template <typename T>
struct objectfile_image : image {
    explicit objectfile_image(std::unique_ptr<T> ptr)
	: arch_(image_arch(*ptr))
	, entry_(image_entry(*ptr))
	, segments_(seg::read(*ptr))
	, symbols_(sym::read(*ptr))
	, sections_(sec::read(*ptr))
	, binary_(move(ptr))
    {}
    std::string arch() const { return arch_; }
    uint64_t entry() const { return entry_; }
    const std::vector<seg::segment>& segments() const { return segments_; }
    const std::vector<sym::symbol>& symbols() const { return symbols_; }
    const std::vector<sec::section>& sections() const { return sections_; }
protected:
    std::string arch_;
    uint64_t entry_;
    std::vector<seg::segment> segments_;
    std::vector<sym::symbol> symbols_;
    std::vector<sec::section> sections_;
    std::unique_ptr<T> binary_;
};

template <typename T>
image* create_image(std::unique_ptr<object::Binary> binary) {
    if (std::unique_ptr<T> ptr =
	dynamic_unique_ptr_cast<T, object::Binary>(move(binary))) {
	return new objectfile_image<T>(move(ptr));
    }
    llvm_binary_fail("Unrecognized object format");
}

image* create_image_elf(std::unique_ptr<object::Binary> binary) {
    if (isa<ELF32LEObjectFile>(*binary))
	return create_image<ELF32LEObjectFile>(move(binary));

    if (isa<ELF32BEObjectFile>(*binary))
        return create_image<ELF32BEObjectFile>(move(binary));

    if (isa<ELF64LEObjectFile>(*binary))
        return create_image<ELF64LEObjectFile>(move(binary));

    if (isa<ELF64BEObjectFile>(*binary))
        return create_image<ELF64BEObjectFile>(move(binary));
    llvm_binary_fail("Unrecognized ELF format");
}

image* create_image_obj(std::unique_ptr<object::Binary> binary) {
    if (binary->isCOFF())
	return create_image<COFFObjectFile>(move(binary));
    if (binary->isELF())
	return create_image_elf(move(binary));
    if (binary->isMachO())
	return create_image<MachOObjectFile>(move(binary));
    llvm_binary_fail("Unrecognized object format");
}

image* create_image_arch(std::unique_ptr<object::Binary> binary) {
    llvm_binary_fail("Archive loading unimplemented");
}

image* create(std::unique_ptr<object::Binary> binary) {
    if (isa<Archive>(*binary))
	return create_image_arch(move(binary));
    if (isa<ObjectFile>(*binary))
	return create_image_obj(move(binary));
    llvm_binary_fail("Unrecognized binary format");
}

image* create(const char* data, std::size_t size) {
    StringRef data_ref(data, size);
    #if LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR == 8

    MemoryBufferRef buf(data_ref, "binary");
    auto binary = createBinary(buf);
    if (error_code ec = binary.getError())
	llvm_binary_fail(ec);
    return create(move(*binary));

    #else

    MemoryBuffer* buff(MemoryBuffer::getMemBufferCopy(data_ref, "binary"));
    OwningPtr<object::Binary> bin;
    if (error_code ec = createBinary(buff, bin))
        llvm_binary_fail(ec);
    std::unique_ptr<object::Binary> binary(bin.take());
    return create(move(binary));
    #endif
}

} //namespace img

#endif //LLVM_BINARY_HPP
