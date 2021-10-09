#include "libpstack/dwarf.h"
#include <set>

namespace Dwarf {

class DIE::Raw {
    Raw() = delete;
    Raw(const Raw &) = delete;
    const Abbreviation *type;
    std::vector<DIE::Attribute::Value> values;
    Elf::Off parent; // 0 implies we do not yet know the parent's offset.
    Elf::Off firstChild;
    Elf::Off nextSibling;
public:
    Raw(Unit *, DWARFReader &, size_t, Elf::Off parent);
    ~Raw();
    // Mostly, Raw DIEs are hidden from everything. DIE needs access though
    friend class DIE;
    static std::shared_ptr<Raw> decode(Unit *unit, const DIE &parent, Elf::Off offset);
};

DIE
DIE::firstChild() const
{
    return unit->offsetToDIE(*this, raw->firstChild);
}

DIE
DIE::nextSibling(const DIE &parent) const
{

    if (raw->nextSibling == 0) {
        // Need to work out what the next sibling is, and we don't have DW_AT_sibling
        // Run through all our children. decodeEntries will update the
        // parent's (our) nextSibling.
        std::shared_ptr<Raw> last = nullptr;
        for (auto &it : children())
            last = it.raw;
        if (last)
            last->nextSibling = 0;
    }
    return unit->offsetToDIE(parent, raw->nextSibling);
}

ContainsAddr
DIE::containsAddress(Elf::Addr addr) const
{
    auto low = attribute(DW_AT_low_pc, true);
    auto high = attribute(DW_AT_high_pc, true);

    if (low.valid() && high.valid()) {
        // Simple case - the DIE has a low and high address. Just see if the
        // addr is in that range
        Elf::Addr start, end;
        switch (low.form()) {
            case DW_FORM_addr:
                start = uintmax_t(low);
                break;
            default:
                abort();
                break;
        }

        switch (high.form()) {
            case DW_FORM_addr:
                end = uintmax_t(high);
                break;
            case DW_FORM_data1:
            case DW_FORM_data2:
            case DW_FORM_data4:
            case DW_FORM_data8:
            case DW_FORM_udata:
                end = start + uintmax_t(high);
                break;
            default:
                abort();
        }
        return start <= addr && end > addr ? ContainsAddr::YES : ContainsAddr::NO;
    }

    // We may have .debug_ranges or .debug_rnglists - see if there's a
    // DW_AT_ranges attr.
    Elf::Addr base = low.valid() ? uintmax_t(low) : 0;
    auto ranges = attribute(DW_AT_ranges, true);
    if (ranges.valid()) {
        // Iterate over the ranges, and see if the address lies inside.
        for (auto &range : Ranges(ranges))
            if (range.first + base <= addr && addr <= range.second + base )
                return ContainsAddr::YES;
        return ContainsAddr::NO;
    }
    return ContainsAddr::UNKNOWN;
}

DIE::Attribute
DIE::attribute(AttrName name, bool local) const
{
    auto loc = raw->type->attrName2Idx.find(name);
    if (loc != raw->type->attrName2Idx.end())
        return Attribute(*this, &raw->type->forms.at(loc->second));

    // If we have attributes of any of these types, we can look for other
    // attributes in the referenced entry.
    static std::set<AttrName> derefs = {
        DW_AT_abstract_origin,
        DW_AT_specification
    };

    // don't dereference declarations, or any types that provide dereference aliases.
    if (!local && name != DW_AT_declaration && derefs.find(name) == derefs.end()) {
        for (auto alt : derefs) {
            const auto &ao = DIE(attribute(alt));
            if (ao && ao.raw != raw)
                return ao.attribute(name);
        }
    }
    return Attribute();
}

std::string
DIE::name() const
{
    auto attr = attribute(DW_AT_name);
    return attr.valid() ? std::string(attr) : "";
}

DIE::Raw::Raw(Unit *unit, DWARFReader &r, size_t abbrev, Elf::Off parent_)
    : type(unit->findAbbreviation(abbrev))
    , parent(parent_)
    , firstChild(0)
    , nextSibling(0)
{
    size_t i = 0;
    values.reserve(type->forms.size());
    for (auto &form : type->forms) {
        values.emplace_back(r, form, values[i], unit);
        if (int(i) == type->nextSibIdx)
            nextSibling = values[i].sdata + unit->offset;
        ++i;
    }
    if (type->hasChildren) {
        // If the type has children, last offset read is the first child.
        firstChild = r.getOffset();
    } else {
        nextSibling = r.getOffset(); // we have no children, so next DIE is next sib
        firstChild = 0; // no children.
    }
}

DIE::Attribute::Value::Value(DWARFReader &r, const FormEntry &forment, DIE::Attribute::Value &value, Unit *unit)
{
    switch (forment.form) {

    case DW_FORM_GNU_strp_alt: {
        value.addr = r.getint(unit->dwarfLen);
        break;
    }

    case DW_FORM_strp:
    case DW_FORM_line_strp:
        value.addr = r.getint(unit->version <= 2 ? 4 : unit->dwarfLen);
        break;

    case DW_FORM_GNU_ref_alt:
        value.addr = r.getuint(unit->dwarfLen);
        break;

    case DW_FORM_addr:
        value.addr = r.getuint(unit->addrlen);
        break;

    case DW_FORM_data1:
        value.udata = r.getu8();
        break;

    case DW_FORM_data2:
        value.udata = r.getu16();
        break;

    case DW_FORM_data4:
        value.udata = r.getu32();
        break;

    case DW_FORM_data8:
        value.udata = r.getuint(8);
        break;

    case DW_FORM_sdata:
        value.sdata = r.getsleb128();
        break;

    case DW_FORM_udata:
        value.udata = r.getuleb128();
        break;

    // offsets in various sections...
    case DW_FORM_strx:
    case DW_FORM_loclistx:
    case DW_FORM_rnglistx:
    case DW_FORM_addrx:
    case DW_FORM_ref_udata:
        value.addr = r.getuleb128();
        break;

    case DW_FORM_strx1:
    case DW_FORM_addrx1:
    case DW_FORM_ref1:
        value.addr = r.getu8();
        break;

    case DW_FORM_strx2:
    case DW_FORM_ref2:
        value.addr = r.getu16();
        break;

    case DW_FORM_addrx3:
    case DW_FORM_strx3:
        value.addr = r.getuint(3);
        break;

    case DW_FORM_strx4:
    case DW_FORM_addrx4:
    case DW_FORM_ref4:
        value.addr = r.getu32();
        break;

    case DW_FORM_ref_addr:
        value.addr = r.getuint(unit->dwarfLen);
        break;

    case DW_FORM_ref8:
        value.addr = r.getuint(8);
        break;

    case DW_FORM_string:
        value.addr = r.getOffset();
        r.getstring();
        break;

    case DW_FORM_block1:
        value.block = new Block();
        value.block->length = r.getu8();
        value.block->offset = r.getOffset();
        r.skip(value.block->length);
        break;

    case DW_FORM_block2:
        value.block = new Block();
        value.block->length = r.getu16();
        value.block->offset = r.getOffset();
        r.skip(value.block->length);
        break;

    case DW_FORM_block4:
        value.block = new Block();
        value.block->length = r.getu32();
        value.block->offset = r.getOffset();
        r.skip(value.block->length);
        break;

    case DW_FORM_exprloc:
    case DW_FORM_block:
        value.block = new Block();
        value.block->length = r.getuleb128();
        value.block->offset = r.getOffset();
        r.skip(value.block->length);
        break;

    case DW_FORM_flag:
        value.flag = r.getu8() != 0;
        break;

    case DW_FORM_flag_present:
        value.flag = true;
        break;

    case DW_FORM_sec_offset:
        value.addr = r.getint(unit->dwarfLen);
        break;

    case DW_FORM_ref_sig8:
        value.signature = r.getuint(8);
        break;

    case DW_FORM_implicit_const:
        value.sdata = forment.value;
        break;

    default:
        value.addr = 0;
        abort();
        break;
    }
}

DIE::Raw::~Raw()
{
    int i = 0;
    for (auto &forment : type->forms) {
        switch (forment.form) {
            case DW_FORM_exprloc:
            case DW_FORM_block:
            case DW_FORM_block1:
            case DW_FORM_block2:
            case DW_FORM_block4:
                delete values[i].block;
                break;
            default:
                break;
        }
        ++i;
    }
}

static void walk(const DIE & die) { for (auto c : die.children()) { walk(c); } };
Elf::Off
DIE::getParentOffset() const
{
    if (raw->parent == 0 && !unit->isRoot(*this)) {
        // This DIE has a parent, but we did not know where it was when we
        // decoded it. We have to search for the parent in the tree. We could
        // limit our search a bit, but the easiest thing to do is just walk the
        // tree from the root down. (This also fixes the problem for any other
        // dies in the same unit.)
        if (verbose)
            *debug << "warning: no parent offset "
                << "for die " << name()
                << " at offset " << offset
                << " in unit " << unit->name()
                << " of " << *unit->dwarf->elf->io
                << ", need to do full walk of DIE tree"
                << std::endl;
        walk(unit->root());
        assert(raw->parent != 0);
    }
    return raw->parent;
}

std::shared_ptr<DIE::Raw>
DIE::decode(Unit *unit, const DIE &parent, Elf::Off offset)
{
    DWARFReader r(unit->dwarf->debugInfo, offset);
    size_t abbrev = r.getuleb128();
    if (abbrev == 0) {
        // If we get to the terminator, then we now know the parent's nextSibling:
        // update it now.
        if (parent)
            parent.raw->nextSibling = r.getOffset();
        return nullptr;
    }
    return std::make_shared<DIE::Raw>(unit, r, abbrev, parent.getOffset());
}

DIE::Children::const_iterator &DIE::Children::const_iterator::operator++() {
    currentDIE = currentDIE.nextSibling(parent);
    // if we loaded the child by a direct refrence into the middle of the
    // unit, (and hence didn't know the parent at the time), take the
    // opportunity to update its parent pointer
    if (currentDIE && parent && currentDIE.raw->parent == 0)
        currentDIE.raw->parent = parent.offset;
    return *this;
}

DIE::Children::const_iterator::const_iterator(const DIE &first, const DIE & parent_)
    : parent(parent_)
    , currentDIE(first)
{
    // As above, take the opportunity to update the current DIE's parent field
    // if it has not already been decided.
    if (currentDIE && parent && currentDIE.raw->parent == 0)
        currentDIE.raw->parent = parent.offset;
}

AttrName
DIE::Attribute::name() const
{
    size_t off = formp - &die.raw->type->forms[0];
    for (auto ent : die.raw->type->attrName2Idx) {
        if (ent.second == off)
            return ent.first;
    }
    return DW_AT_none;
}

DIE::Attribute::operator intmax_t() const
{
    if (!valid())
        return 0;
    switch (formp->form) {
    case DW_FORM_data1:
    case DW_FORM_data2:
    case DW_FORM_data4:
    case DW_FORM_data8:
    case DW_FORM_sdata:
    case DW_FORM_udata:
    case DW_FORM_implicit_const:
        return value().sdata;
    case DW_FORM_sec_offset:
        return value().addr;
    default:
        abort();
    }
}

DIE::Attribute::operator uintmax_t() const
{
    if (!valid())
        return 0;
    switch (formp->form) {
    case DW_FORM_data1:
    case DW_FORM_data2:
    case DW_FORM_data4:
    case DW_FORM_data8:
    case DW_FORM_udata:
    case DW_FORM_implicit_const:
        return value().udata;
    case DW_FORM_addr:
    case DW_FORM_sec_offset:
        return value().addr;
    default:
        abort();
    }
}

DIE::Attribute::operator const Ranges&() const
{
    auto val = value().addr;

    Ranges &ranges = die.unit->rangesForOffset[val];

    if (!ranges.isNew)
        return ranges;

    ranges.isNew = false;

    if (die.unit->version < 5) {
        // DWARF4 units use debug_ranges
        DWARFReader reader(die.unit->dwarf->debugRanges, value().addr);
        for (;;) {
            auto start = reader.getuint(sizeof (Elf::Addr));
            auto end = reader.getuint(sizeof (Elf::Addr));
            if (start == 0 && end == 0)
                break;
            ranges.emplace_back(std::make_pair(start, end));
        }
    } else {
        // DWARF5 units use debug_rnglists.
        Elf::Off offset = value().addr;

        // Offset by rnglists_base in the root DIE.
        auto root = die.unit->root();
        auto attr = root.attribute(DW_AT_rnglists_base);
        if (attr.valid())
            offset += uintmax_t(attr);

        auto rnglists = die.unit->dwarf->elf->sectionReader(
                                    ".debug_rnglists", ".zdebug_rnglists", nullptr);
        auto addrs = die.unit->dwarf->elf->sectionReader(
                                    ".debug_addr", ".zdebug_addr", nullptr);
        DWARFReader r(rnglists, offset);

        uintmax_t base = 0;
        for (bool done = false; !done;) {
            auto entryType = DW_RLE(r.getu8());
            switch (entryType) {
                case DW_RLE_end_of_list:
                    done = true;
                    break;

                case DW_RLE_base_addressx: {
                    /* auto baseidx = */ r.getuleb128();
                    abort();
                    break;
                }

                case DW_RLE_startx_endx: {
                    /* auto startx = */ r.getuleb128();
                    /* auto endx = */ r.getuleb128();
                    abort();
                    break;
                }

                case DW_RLE_startx_length: {
                    /* auto starti = */ r.getuleb128();
                    /* auto len = */ r.getuleb128();
                    abort();
                    break;
                }

                case DW_RLE_offset_pair: {
                    auto offstart = r.getuleb128();
                    auto offend = r.getuleb128();
                    ranges.emplace_back(offstart + base, offend + base);
                    break;
                }

                case DW_RLE_base_address:
                    base = r.getuint(die.unit->addrlen);
                    break;

                case DW_RLE_start_end: {
                    auto start = r.getuint(die.unit->addrlen);
                    auto end = r.getuint(die.unit->addrlen);
                    ranges.emplace_back(start, end);
                    break;
                }
                case DW_RLE_start_length: {
                    auto start = r.getuint(die.unit->addrlen);
                    auto len = r.getuleb128();
                    ranges.emplace_back(start, start + len);
                    break;
                }
                default:
                    abort();
            }
        }
    }
    return ranges;
}

DIE::Attribute::operator std::string() const
{
    if (!valid())
        return "";
    const Info *dwarf = die.unit->dwarf;
    assert(dwarf != nullptr);
    switch (formp->form) {

        case DW_FORM_GNU_strp_alt: {
            const auto &alt = dwarf->getAltDwarf();
            if (!alt)
                return "(alt string table unavailable)";
            auto &strs = alt->debugStrings;
            if (!strs)
                return "(alt string table unavailable)";
            return strs->readString(value().addr);
        }
        case DW_FORM_strp:
            return dwarf->debugStrings->readString(value().addr);

        case DW_FORM_line_strp:
            return dwarf->debugLineStrings->readString(value().addr);

        case DW_FORM_string:
            return die.unit->dwarf->debugInfo->readString(value().addr);

        case DW_FORM_strx1:
        case DW_FORM_strx2:
        case DW_FORM_strx3:
        case DW_FORM_strx4:
        case DW_FORM_strx:
            return dwarf->strx(*die.unit, value().addr);

        default:
            abort();
    }
}

DIE::Attribute::operator DIE() const
{
    if (!valid())
        return DIE();

    const Info *dwarf = die.unit->dwarf;
    Elf::Off off;
    switch (formp->form) {
        case DW_FORM_ref_addr:
            off = value().addr;
            break;
        case DW_FORM_ref_udata:
        case DW_FORM_ref1:
        case DW_FORM_ref2:
        case DW_FORM_ref4:
        case DW_FORM_ref8:
            off = value().addr + die.unit->offset;
            break;
        case DW_FORM_GNU_ref_alt: {
            dwarf = dwarf->getAltDwarf().get();
            if (dwarf == nullptr)
                throw (Exception() << "no alt reference");
            off = value().addr;
            break;
        }
        default:
            abort();
            break;
    }

    // Try this unit first (if we're dealing with the same Info)
    if (dwarf == die.unit->dwarf && die.unit->offset <= off && die.unit->end > off) {
        const auto otherEntry = die.unit->offsetToDIE(DIE(), off);
        if (otherEntry)
            return otherEntry;
    }

    // Nope - try other units.
    return dwarf->offsetToDIE(off);
}

DIE
DIE::findEntryForAddr(Elf::Addr address, Tag t, bool skipStart)
{
    switch (containsAddress(address)) {
        case ContainsAddr::NO:
            return DIE();
        case ContainsAddr::YES:
            if (!skipStart && tag() == t)
                return *this;
            /* FALLTHRU */
        case ContainsAddr::UNKNOWN:
            for (auto child : children()) {
                auto descendent = child.findEntryForAddr(address, t, false);
                if (descendent)
                    return descendent;
            }
            return DIE();
    }
    return DIE();
}

DIE::Children::const_iterator
DIE::Children::begin() const {
    return const_iterator(parent.firstChild(), parent);
}

DIE::Children::const_iterator
DIE::Children::end() const {
    return const_iterator(DIE(), parent);
}

std::pair<AttrName, DIE::Attribute>
DIE::Attributes::const_iterator::operator *() const {
    return std::make_pair(
            rawIter->first,
            Attribute(die, &die.raw->type->forms[rawIter->second]));
}

DIE::Attributes::const_iterator
DIE::Attributes::begin() const {
    return const_iterator(die, die.raw->type->attrName2Idx.begin());
}

DIE::Attributes::const_iterator
DIE::Attributes::end() const {
    return const_iterator(die, die.raw->type->attrName2Idx.end());
}

const DIE::Attribute::Value &DIE::Attribute::value() const {
    return die.raw->values.at(formp - &die.raw->type->forms[0]);
}

Tag DIE::tag() const {
    return raw->type->tag;
}

bool DIE::hasChildren() const {
    return raw->type->hasChildren;
}

std::string
DIE::typeName(const DIE &type)
{
    if (!*this)
        return "void";

    const auto &name = type.name();
    if (name != "")
        return name;
    auto base = DIE(type.attribute(DW_AT_type));
    std::string s, sep;
    switch (type.tag()) {
        case DW_TAG_pointer_type:
            return typeName(base) + " *";
        case DW_TAG_const_type:
            return typeName(base) + " const";
        case DW_TAG_volatile_type:
            return typeName(base) + " volatile";
        case DW_TAG_subroutine_type:
            s = typeName(base) + "(";
            sep = "";
            for (auto arg : type.children()) {
                if (arg.tag() != DW_TAG_formal_parameter)
                    continue;
                s += sep;
                s += typeName(DIE(arg.attribute(DW_AT_type)));
                sep = ", ";
            }
            s += ")";
            return s;
        case DW_TAG_reference_type:
            return typeName(base) + "&";
        default: {
            return stringify("(unhandled tag ", type.tag(), ")");
        }
    }
}

}
