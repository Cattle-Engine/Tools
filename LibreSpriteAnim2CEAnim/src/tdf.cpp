#include "tdf.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <tuple>

namespace CE::TDF {
namespace {

constexpr uint8_t kMagic[3] = { 'T', 'D', 'F' };
constexpr uint8_t kObjectVersion = 0x11;

[[noreturn]] void throwFormat(const char* msg) {
    throw std::runtime_error(std::string("TDF: ") + msg);
}

void writeU32(std::vector<uint8_t>& out, uint32_t v) {
    uint8_t b[4];
    std::memcpy(b, &v, 4);
    out.insert(out.end(), b, b + 4);
}

template <typename T>
void writePod(std::vector<uint8_t>& out, const T& value) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

uint32_t readU32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

template <typename T>
T readPod(const uint8_t* p) {
    T value;
    std::memcpy(&value, p, sizeof(T));
    return value;
}

void requireDataSize(const Value& v, size_t expected, const char* msg) {
    if (v.data.size() != expected) {
        throwFormat(msg);
    }
}

#if defined(TDF_MODE_CE)
void readExact(::VirtualFile* file, void* buffer, size_t size) {
    if (!file || !file->sdl_stream) {
        throwFormat("invalid stream");
    }
    if (size == 0) {
        return;
    }

    const size_t read = SDL_ReadIO(file->sdl_stream, buffer, size);
    if (read != size) {
        throwFormat("unexpected EOF in stream");
    }
}
#endif

std::vector<std::string> splitPath(const std::string& s, char sep) {
    std::vector<std::string> parts;
    std::string cur;

    for (char c : s) {
        if (c == sep) {
            if (cur.empty()) {
                throwFormat("empty path segment");
            }
            parts.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }

    if (cur.empty()) {
        throwFormat("empty path segment");
    }
    parts.push_back(std::move(cur));
    return parts;
}

std::vector<uint8_t> serializeToBytes(const File& file, uint8_t version) {
    std::vector<uint8_t> bytes;
    bytes.insert(bytes.end(), kMagic, kMagic + 3);
    bytes.push_back(version);

    std::vector<uint8_t> index;
    std::vector<uint8_t> data;

    for (const auto& [key, val] : file.entries) {
        if (key.empty() || key.size() > 255) {
            throwFormat("key length must be 1..255");
        }
        if (version < kObjectVersion && (val.type == Type::Object || val.type == Type::ArrObject)) {
            throwFormat("Object/ArrObject requires version >= 0x11");
        }

        const uint32_t offset = static_cast<uint32_t>(data.size());
        index.push_back(static_cast<uint8_t>(key.size()));
        index.insert(index.end(), key.begin(), key.end());
        index.push_back(static_cast<uint8_t>(val.type));
        writeU32(index, offset);
        data.insert(data.end(), val.data.begin(), val.data.end());
    }

    index.push_back(0x00);
    bytes.insert(bytes.end(), index.begin(), index.end());
    bytes.insert(bytes.end(), data.begin(), data.end());
    return bytes;
}

File parseFromBytes(const uint8_t* data, size_t size) {
    if (size < 4) {
        throwFormat("buffer too small");
    }
    if (data[0] != kMagic[0] || data[1] != kMagic[1] || data[2] != kMagic[2]) {
        throwFormat("invalid magic");
    }

    size_t pos = 4;
    std::vector<std::tuple<std::string, Type, uint32_t>> index;

    while (true) {
        if (pos >= size) {
            throwFormat("unexpected EOF in index");
        }

        const uint8_t keyLen = data[pos++];
        if (keyLen == 0) {
            break;
        }

        if (pos + keyLen + 1 + 4 > size) {
            throwFormat("index overflow");
        }

        std::string key(
            reinterpret_cast<const char*>(data + pos),
            reinterpret_cast<const char*>(data + pos + keyLen)
        );
        pos += keyLen;

        const Type type = static_cast<Type>(data[pos++]);
        const uint32_t offset = readU32(data + pos);
        pos += 4;

        index.emplace_back(std::move(key), type, offset);
    }

    const size_t dataStart = pos;
    File file;

    for (auto& [key, type, offset] : index) {
        size_t p = dataStart + offset;
        if (p > size) {
            throwFormat("offset out of range");
        }

        Value v{ type, {} };

        const auto require = [&](size_t n) {
            if (p + n > size) {
                throwFormat("unexpected EOF in value");
            }
        };

        if (v.type == Type::Null) {
        } else if (v.type == Type::String) {
            do {
                require(1);
                const uint8_t c = data[p++];
                v.data.push_back(c);
                if (c == 0) {
                    break;
                }
            } while (true);
        } else if (v.type == Type::Object) {
            require(4);
            const uint32_t len = readU32(data + p);
            require(4 + len);
            v.data.resize(4 + len);
            std::memcpy(v.data.data(), data + p, 4 + len);
        } else if (File::isArray(v.type)) {
            require(4);
            const uint32_t count = readU32(data + p);

            if (v.type == Type::ArrString) {
                v.data.insert(v.data.end(), data + p, data + p + 4);
                p += 4;

                for (uint32_t i = 0; i < count; ++i) {
                    while (true) {
                        require(1);
                        const uint8_t c = data[p++];
                        v.data.push_back(c);
                        if (c == 0) {
                            break;
                        }
                    }
                }
            } else if (v.type == Type::ArrObject) {
                v.data.insert(v.data.end(), data + p, data + p + 4);
                p += 4;

                for (uint32_t i = 0; i < count; ++i) {
                    require(4);
                    const uint32_t len = readU32(data + p);
                    require(4 + len);
                    v.data.insert(v.data.end(), data + p, data + p + 4 + len);
                    p += 4 + len;
                }
            } else {
                const size_t elemSize = File::elementSize(v.type, File::elementType(v.type));
                require(4 + elemSize * count);
                v.data.resize(4 + elemSize * count);
                std::memcpy(v.data.data(), data + p, v.data.size());
            }
        } else {
            size_t sz = 0;
            switch (v.type) {
                case Type::Bool: sz = 1; break;
                case Type::Int32:
                case Type::UInt32:
                case Type::Float: sz = 4; break;
                default: throwFormat("unknown scalar");
            }

            require(sz);
            v.data.resize(sz);
            std::memcpy(v.data.data(), data + p, sz);
        }

        file.entries.emplace(std::move(key), std::move(v));
    }

    return file;
}

File decodeObjectValue(const Value& v) {
    if (v.type != Type::Object) {
        throwFormat("value is not Object");
    }
    if (v.data.size() < 4) {
        throwFormat("object too small");
    }

    const uint32_t len = readU32(v.data.data());
    if (v.data.size() != 4ull + len) {
        throwFormat("object size mismatch");
    }

    return parseFromBytes(v.data.data() + 4, len);
}

Value encodeObjectValue(const File& obj, uint8_t version) {
    const std::vector<uint8_t> blob = serializeToBytes(obj, version);
    Value v{ Type::Object, {} };
    writeU32(v.data, static_cast<uint32_t>(blob.size()));
    v.data.insert(v.data.end(), blob.begin(), blob.end());
    return v;
}

std::vector<std::string> decodeStringArray(const Value& v) {
    if (v.type != Type::ArrString) {
        throwFormat("not ArrString");
    }
    if (v.data.size() < 4) {
        throwFormat("ArrString too small");
    }

    const uint32_t count = readU32(v.data.data());
    std::vector<std::string> out;
    out.reserve(count);

    size_t p = 4;
    for (uint32_t i = 0; i < count; ++i) {
        std::string s;
        while (true) {
            if (p >= v.data.size()) {
                throwFormat("ArrString missing terminator");
            }
            const uint8_t c = v.data[p++];
            if (c == 0) {
                break;
            }
            s.push_back(static_cast<char>(c));
        }
        out.push_back(std::move(s));
    }

    return out;
}

Value encodeStringArray(const std::vector<std::string>& arr) {
    Value v{ Type::ArrString, {} };
    writeU32(v.data, static_cast<uint32_t>(arr.size()));

    for (const auto& s : arr) {
        v.data.insert(v.data.end(), s.begin(), s.end());
        v.data.push_back(0);
    }

    return v;
}

std::vector<File> decodeObjectArray(const Value& v) {
    if (v.type != Type::ArrObject) {
        throwFormat("not ArrObject");
    }
    if (v.data.size() < 4) {
        throwFormat("ArrObject too small");
    }

    const uint32_t count = readU32(v.data.data());
    std::vector<File> out;
    out.reserve(count);

    size_t p = 4;
    for (uint32_t i = 0; i < count; ++i) {
        if (p + 4 > v.data.size()) {
            throwFormat("ArrObject truncated");
        }

        const uint32_t len = readU32(v.data.data() + p);
        p += 4;

        if (p + len > v.data.size()) {
            throwFormat("ArrObject overflow");
        }

        out.push_back(parseFromBytes(v.data.data() + p, len));
        p += len;
    }

    return out;
}

Value encodeObjectArray(const std::vector<File>& arr, uint8_t version) {
    Value v{ Type::ArrObject, {} };
    writeU32(v.data, static_cast<uint32_t>(arr.size()));

    for (const auto& obj : arr) {
        const auto blob = serializeToBytes(obj, version);
        writeU32(v.data, static_cast<uint32_t>(blob.size()));
        v.data.insert(v.data.end(), blob.begin(), blob.end());
    }

    return v;
}

template <typename T>
std::vector<T> decodeFixedArray(const Value& v, Type expectedType) {
    if (v.type != expectedType) {
        throwFormat("unexpected array type");
    }
    if (v.data.size() < 4) {
        throwFormat("array too small");
    }

    const uint32_t count = readU32(v.data.data());
    const size_t bytes = sizeof(T) * static_cast<size_t>(count);
    if (v.data.size() != 4 + bytes) {
        throwFormat("array size mismatch");
    }

    std::vector<T> out(count);
    if (bytes > 0) {
        std::memcpy(out.data(), v.data.data() + 4, bytes);
    }
    return out;
}

Value encodeBoolArray(const std::vector<bool>& arr) {
    Value v{ Type::ArrBool, {} };
    writeU32(v.data, static_cast<uint32_t>(arr.size()));
    v.data.reserve(4 + arr.size());
    for (bool value : arr) {
        v.data.push_back(value ? 1 : 0);
    }
    return v;
}

template <typename T>
Value encodeFixedArray(Type type, const std::vector<T>& arr) {
    Value v{ type, {} };
    writeU32(v.data, static_cast<uint32_t>(arr.size()));
    if (!arr.empty()) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(arr.data());
        v.data.insert(v.data.end(), bytes, bytes + sizeof(T) * arr.size());
    }
    return v;
}

Value makeSingleElementArray(const Value& val) {
    switch (val.type) {
        case Type::Bool: return File::makeBoolArray({ File::readBool(val) });
        case Type::Int32: return File::makeIntArray({ File::readInt(val) });
        case Type::UInt32: return File::makeUIntArray({ File::readUInt(val) });
        case Type::Float: return File::makeFloatArray({ File::readFloat(val) });
        case Type::String: return File::makeStringArray({ File::readString(val) });
        case Type::Object: return File::makeObjectArray({ File::readObject(val) }, kObjectVersion);
        default: throwFormat("unsupported array element type");
    }
}

void appendArrayValue(Value& array, const Value& val) {
    if (!File::isArray(array.type)) {
        throwFormat("value is not array");
    }
    if (File::elementType(array.type) != val.type) {
        throwFormat("array element type mismatch");
    }

    switch (array.type) {
        case Type::ArrBool: {
            auto items = File::readBoolArray(array);
            items.push_back(File::readBool(val));
            array = File::makeBoolArray(items);
            return;
        }
        case Type::ArrInt32: {
            auto items = File::readIntArray(array);
            items.push_back(File::readInt(val));
            array = File::makeIntArray(items);
            return;
        }
        case Type::ArrUInt32: {
            auto items = File::readUIntArray(array);
            items.push_back(File::readUInt(val));
            array = File::makeUIntArray(items);
            return;
        }
        case Type::ArrFloat: {
            auto items = File::readFloatArray(array);
            items.push_back(File::readFloat(val));
            array = File::makeFloatArray(items);
            return;
        }
        case Type::ArrString: {
            auto items = File::readStringArray(array);
            items.push_back(File::readString(val));
            array = File::makeStringArray(items);
            return;
        }
        case Type::ArrObject: {
            auto items = File::readObjectArray(array);
            items.push_back(File::readObject(val));
            array = File::makeObjectArray(items, kObjectVersion);
            return;
        }
        default:
            throwFormat("unknown array type");
    }
}

void eraseArrayValue(Value& array, size_t index) {
    if (!File::isArray(array.type)) {
        throwFormat("value is not array");
    }
    if (index >= File::arraySize(array)) {
        throwFormat("array index out of range");
    }

    switch (array.type) {
        case Type::ArrBool: {
            auto items = File::readBoolArray(array);
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
            array = File::makeBoolArray(items);
            return;
        }
        case Type::ArrInt32: {
            auto items = File::readIntArray(array);
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
            array = File::makeIntArray(items);
            return;
        }
        case Type::ArrUInt32: {
            auto items = File::readUIntArray(array);
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
            array = File::makeUIntArray(items);
            return;
        }
        case Type::ArrFloat: {
            auto items = File::readFloatArray(array);
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
            array = File::makeFloatArray(items);
            return;
        }
        case Type::ArrString: {
            auto items = File::readStringArray(array);
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
            array = File::makeStringArray(items);
            return;
        }
        case Type::ArrObject: {
            auto items = File::readObjectArray(array);
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
            array = File::makeObjectArray(items, kObjectVersion);
            return;
        }
        default:
            throwFormat("unknown array type");
    }
}

bool tryGetPathRecursive(const File& file, const std::vector<std::string>& parts, size_t index, Value& out) {
    const auto it = file.entries.find(parts[index]);
    if (it == file.entries.end()) {
        return false;
    }
    if (index + 1 == parts.size()) {
        out = it->second;
        return true;
    }
    if (it->second.type != Type::Object) {
        return false;
    }
    const File child = decodeObjectValue(it->second);
    return tryGetPathRecursive(child, parts, index + 1, out);
}

bool removePathRecursive(File& file, const std::vector<std::string>& parts, size_t index) {
    auto it = file.entries.find(parts[index]);
    if (it == file.entries.end()) {
        return false;
    }
    if (index + 1 == parts.size()) {
        file.entries.erase(it);
        return true;
    }
    if (it->second.type != Type::Object) {
        return false;
    }

    File child = decodeObjectValue(it->second);
    const bool removed = removePathRecursive(child, parts, index + 1);
    if (removed) {
        it->second = encodeObjectValue(child, kObjectVersion);
    }
    return removed;
}

void setPathRecursive(File& file, const std::vector<std::string>& parts, size_t index, const Value& val) {
    if (index + 1 == parts.size()) {
        file.entries[parts[index]] = val;
        return;
    }

    Value& slot = file.entries[parts[index]];
    if (slot.type == Type::Null && slot.data.empty()) {
        slot = encodeObjectValue(File{}, kObjectVersion);
    } else if (slot.type != Type::Object) {
        throwFormat("path segment is not Object");
    }

    File child = decodeObjectValue(slot);
    setPathRecursive(child, parts, index + 1, val);
    slot = encodeObjectValue(child, kObjectVersion);
}

} // namespace

#if defined(TDF_MODE_CE)
void File::save(VFS::VFS& vfs, const std::string& path, uint8_t version) const {
    static_cast<void>(vfs);
    static_cast<void>(path);
    static_cast<void>(version);
    throw std::runtime_error("TDF: write is not supported in TDF_MODE_CE");
}

void File::load(VFS::VFS& vfs, const std::string& path) {
    ::VirtualFile* f = vfs.OpenFile(path.c_str());
    if (!f) {
        throw std::runtime_error("TDF: VFS open fail");
    }

    const size_t size = static_cast<size_t>(f->size);
    std::vector<uint8_t> buffer(size);

    const size_t read = vfs.ReadFile(f, buffer.data(), buffer.size());
    vfs.CloseFile(f);

    if (read != buffer.size()) {
        throw std::runtime_error("TDF: incomplete read");
    }

    *this = parseFromBytes(buffer.data(), buffer.size());
}
#endif

#if defined(TDF_MODE_EXTERN)
void File::save(const std::filesystem::path& path, uint8_t version) const {
    const auto bytes = serializeToBytes(*this, version);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        throw std::runtime_error("TDF: filesystem write fail");
    }

    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    if (!out) {
        throw std::runtime_error("TDF: incomplete write");
    }
}

void File::load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("TDF: filesystem open fail");
    }

    const auto size = std::filesystem::file_size(path);
    std::vector<uint8_t> buffer(static_cast<size_t>(size));

    if (!buffer.empty()) {
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    }

    if (!in) {
        throw std::runtime_error("TDF: incomplete read");
    }

    *this = parseFromBytes(buffer.data(), buffer.size());
}
#endif

void File::set(const std::string& key, const Value& val) {
    entries[key] = val;
}

bool File::remove(const std::string& key) {
    return entries.erase(key) > 0;
}

bool File::has(const std::string& key) const {
    return entries.find(key) != entries.end();
}

bool File::hasPath(const std::string& path, char separator) const {
    Value out;
    return tryGetPath(path, out, separator);
}

bool File::tryGetPath(const std::string& path, Value& out, char separator) const {
    const auto parts = splitPath(path, separator);
    return tryGetPathRecursive(*this, parts, 0, out);
}

bool File::removePath(const std::string& path, char separator) {
    const auto parts = splitPath(path, separator);
    return removePathRecursive(*this, parts, 0);
}

void File::setPath(const std::string& path, const Value& val, char separator) {
    const auto parts = splitPath(path, separator);
    setPathRecursive(*this, parts, 0, val);
}

void File::appendToArray(const std::string& key, const Value& val) {
    auto it = entries.find(key);
    if (it == entries.end()) {
        entries.emplace(key, makeSingleElementArray(val));
        return;
    }

    appendArrayValue(it->second, val);
}

void File::deleteFromArray(const std::string& key, size_t index) {
    auto it = entries.find(key);
    if (it == entries.end()) {
        throwFormat("array entry missing");
    }

    eraseArrayValue(it->second, index);
}

Value File::makeNull() {
    return Value{ Type::Null, {} };
}

Value File::makeBool(bool v) {
    return Value{ Type::Bool, { static_cast<uint8_t>(v ? 1 : 0) } };
}

Value File::makeInt(int32_t v) {
    Value out{ Type::Int32, {} };
    writePod(out.data, v);
    return out;
}

Value File::makeUInt(uint32_t v) {
    Value out{ Type::UInt32, {} };
    writePod(out.data, v);
    return out;
}

Value File::makeFloat(float v) {
    Value out{ Type::Float, {} };
    writePod(out.data, v);
    return out;
}

Value File::makeString(const std::string& s) {
    Value out{ Type::String, {} };
    out.data.insert(out.data.end(), s.begin(), s.end());
    out.data.push_back(0);
    return out;
}

Value File::makeBoolArray(const std::vector<bool>& arr) {
    return encodeBoolArray(arr);
}

Value File::makeIntArray(const std::vector<int32_t>& arr) {
    return encodeFixedArray(Type::ArrInt32, arr);
}

Value File::makeUIntArray(const std::vector<uint32_t>& arr) {
    return encodeFixedArray(Type::ArrUInt32, arr);
}

Value File::makeFloatArray(const std::vector<float>& arr) {
    return encodeFixedArray(Type::ArrFloat, arr);
}

Value File::makeStringArray(const std::vector<std::string>& arr) {
    return encodeStringArray(arr);
}

Value File::makeObject(const File& obj, uint8_t version) {
    return encodeObjectValue(obj, version);
}

Value File::makeObjectArray(const std::vector<File>& arr, uint8_t version) {
    return encodeObjectArray(arr, version);
}

bool File::readBool(const Value& v) {
    if (v.type != Type::Bool) {
        throwFormat("value is not Bool");
    }
    requireDataSize(v, 1, "Bool size mismatch");
    return v.data[0] != 0;
}

int32_t File::readInt(const Value& v) {
    if (v.type != Type::Int32) {
        throwFormat("value is not Int32");
    }
    requireDataSize(v, sizeof(int32_t), "Int32 size mismatch");
    return readPod<int32_t>(v.data.data());
}

uint32_t File::readUInt(const Value& v) {
    if (v.type != Type::UInt32) {
        throwFormat("value is not UInt32");
    }
    requireDataSize(v, sizeof(uint32_t), "UInt32 size mismatch");
    return readPod<uint32_t>(v.data.data());
}

float File::readFloat(const Value& v) {
    if (v.type != Type::Float) {
        throwFormat("value is not Float");
    }
    requireDataSize(v, sizeof(float), "Float size mismatch");
    return readPod<float>(v.data.data());
}

std::string File::readString(const Value& v) {
    if (v.type != Type::String) {
        throwFormat("value is not String");
    }
    if (v.data.empty() || v.data.back() != 0) {
        throwFormat("String missing terminator");
    }
    return std::string(reinterpret_cast<const char*>(v.data.data()));
}

File File::readObject(const Value& v) {
    return decodeObjectValue(v);
}

std::vector<bool> File::readBoolArray(const Value& v) {
    if (v.type != Type::ArrBool) {
        throwFormat("value is not ArrBool");
    }
    if (v.data.size() < 4) {
        throwFormat("ArrBool too small");
    }

    const uint32_t count = readU32(v.data.data());
    if (v.data.size() != 4ull + count) {
        throwFormat("ArrBool size mismatch");
    }

    std::vector<bool> out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        out.push_back(v.data[4 + i] != 0);
    }
    return out;
}

std::vector<int32_t> File::readIntArray(const Value& v) {
    return decodeFixedArray<int32_t>(v, Type::ArrInt32);
}

std::vector<uint32_t> File::readUIntArray(const Value& v) {
    return decodeFixedArray<uint32_t>(v, Type::ArrUInt32);
}

std::vector<float> File::readFloatArray(const Value& v) {
    return decodeFixedArray<float>(v, Type::ArrFloat);
}

std::vector<std::string> File::readStringArray(const Value& v) {
    return decodeStringArray(v);
}

std::vector<File> File::readObjectArray(const Value& v) {
    return decodeObjectArray(v);
}

size_t File::arraySize(const Value& v) {
    if (!isArray(v.type)) {
        throwFormat("value is not array");
    }
    if (v.data.size() < 4) {
        throwFormat("array too small");
    }
    return readU32(v.data.data());
}

Value File::arrayElement(const Value& v, size_t index) {
    if (index >= arraySize(v)) {
        throwFormat("array index out of range");
    }

    switch (v.type) {
        case Type::ArrBool: return makeBool(readBoolArray(v)[index]);
        case Type::ArrInt32: return makeInt(readIntArray(v)[index]);
        case Type::ArrUInt32: return makeUInt(readUIntArray(v)[index]);
        case Type::ArrFloat: return makeFloat(readFloatArray(v)[index]);
        case Type::ArrString: return makeString(readStringArray(v)[index]);
        case Type::ArrObject: return makeObject(readObjectArray(v)[index], kObjectVersion);
        default: throwFormat("unknown array type");
    }
}

#if defined(TDF_MODE_CE)
void File::readValue(::VirtualFile* file, Value& v) {
    switch (v.type) {
        case Type::Null:
            v.data.clear();
            return;
        case Type::Bool: {
            uint8_t raw = 0;
            readExact(file, &raw, 1);
            v.data = { raw };
            return;
        }
        case Type::Int32:
        case Type::UInt32:
        case Type::Float: {
            v.data.resize(4);
            readExact(file, v.data.data(), 4);
            return;
        }
        case Type::String: {
            v.data.clear();
            while (true) {
                uint8_t c = 0;
                readExact(file, &c, 1);
                v.data.push_back(c);
                if (c == 0) {
                    return;
                }
            }
        }
        case Type::Object: {
            uint32_t len = 0;
            readExact(file, &len, 4);
            v.data.clear();
            writeU32(v.data, len);
            if (len > 0) {
                const size_t oldSize = v.data.size();
                v.data.resize(oldSize + len);
                readExact(file, v.data.data() + oldSize, len);
            }
            return;
        }
        case Type::ArrString: {
            uint32_t count = 0;
            readExact(file, &count, 4);
            v.data.clear();
            writeU32(v.data, count);
            for (uint32_t i = 0; i < count; ++i) {
                while (true) {
                    uint8_t c = 0;
                    readExact(file, &c, 1);
                    v.data.push_back(c);
                    if (c == 0) {
                        break;
                    }
                }
            }
            return;
        }
        case Type::ArrObject: {
            uint32_t count = 0;
            readExact(file, &count, 4);
            v.data.clear();
            writeU32(v.data, count);
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t len = 0;
                readExact(file, &len, 4);
                writeU32(v.data, len);
                const size_t oldSize = v.data.size();
                v.data.resize(oldSize + len);
                if (len > 0) {
                    readExact(file, v.data.data() + oldSize, len);
                }
            }
            return;
        }
        case Type::ArrBool:
        case Type::ArrInt32:
        case Type::ArrUInt32:
        case Type::ArrFloat: {
            uint32_t count = 0;
            readExact(file, &count, 4);
            v.data.clear();
            writeU32(v.data, count);
            const size_t elemBytes = elementSize(v.type, elementType(v.type));
            const size_t payloadBytes = elemBytes * static_cast<size_t>(count);
            const size_t oldSize = v.data.size();
            v.data.resize(oldSize + payloadBytes);
            if (payloadBytes > 0) {
                readExact(file, v.data.data() + oldSize, payloadBytes);
            }
            return;
        }
        default:
            throwFormat("unknown value type");
    }
}
#endif

bool File::isArray(Type t) {
    const uint8_t raw = static_cast<uint8_t>(t);
    return raw >= static_cast<uint8_t>(Type::ArrBool) && raw <= static_cast<uint8_t>(Type::ArrObject);
}

Type File::elementType(Type arrType) {
    switch (arrType) {
        case Type::ArrBool: return Type::Bool;
        case Type::ArrInt32: return Type::Int32;
        case Type::ArrUInt32: return Type::UInt32;
        case Type::ArrFloat: return Type::Float;
        case Type::ArrString: return Type::String;
        case Type::ArrObject: return Type::Object;
        default: throwFormat("value is not array");
    }
}

size_t File::elementSize(Type arrType, Type elemType) {
    if (elementType(arrType) != elemType) {
        throwFormat("array element type mismatch");
    }

    switch (elemType) {
        case Type::Bool: return 1;
        case Type::Int32:
        case Type::UInt32:
        case Type::Float: return 4;
        case Type::String:
        case Type::Object: return 0;
        default: throwFormat("unsupported element type");
    }
}

} // namespace CE::TDF
