#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#if !defined(TDF_MODE_CE) && !defined(TDF_MODE_EXTERN)
#define TDF_MODE_CE
#endif

#if defined(TDF_MODE_CE) && defined(TDF_MODE_EXTERN)
#error "TDF_MODE_CE and TDF_MODE_EXTERN are mutually exclusive"
#endif

#if defined(TDF_MODE_CE)
#include "engine/common/fs/vfs.hpp"
#endif

namespace CE::TDF {
    enum class Type : uint8_t {
        Null = 0,
        Bool,
        Int32,
        UInt32,
        Float,
        String,
        Object,

        ArrBool = 0xE0,
        ArrInt32,
        ArrUInt32,
        ArrFloat,
        ArrString,
        ArrObject,
    };

    struct Value {
        Type type = Type::Null;
        std::vector<uint8_t> data;
    };

    struct File {
        std::unordered_map<std::string, Value> entries;

#if defined(TDF_MODE_CE)
        void save(VFS::VFS& vfs, const std::string& path, uint8_t version) const;
        void load(VFS::VFS& vfs, const std::string& path);
#endif
#if defined(TDF_MODE_EXTERN)
        void save(const std::filesystem::path& path, uint8_t version) const;
        void load(const std::filesystem::path& path);
#endif

        void set(const std::string& key, const Value& val);
        bool remove(const std::string& key);
        bool has(const std::string& key) const;

        bool hasPath(const std::string& path, char separator = '/') const;
        bool tryGetPath(const std::string& path, Value& out, char separator = '/') const;
        bool removePath(const std::string& path, char separator = '/');
        void setPath(const std::string& path, const Value& val, char separator = '/');

        void appendToArray(const std::string& key, const Value& val);
        void deleteFromArray(const std::string& key, size_t index);

        static Value makeNull();
        static Value makeBool(bool v);
        static Value makeInt(int32_t v);
        static Value makeUInt(uint32_t v);
        static Value makeFloat(float v);
        static Value makeString(const std::string& s);

        static Value makeBoolArray(const std::vector<bool>& arr);
        static Value makeIntArray(const std::vector<int32_t>& arr);
        static Value makeUIntArray(const std::vector<uint32_t>& arr);
        static Value makeFloatArray(const std::vector<float>& arr);
        static Value makeStringArray(const std::vector<std::string>& arr);

        static Value makeObject(const File& obj, uint8_t version);
        static Value makeObjectArray(const std::vector<File>& arr, uint8_t version);

        static bool readBool(const Value& v);
        static int32_t readInt(const Value& v);
        static uint32_t readUInt(const Value& v);
        static float readFloat(const Value& v);
        static std::string readString(const Value& v);
        static File readObject(const Value& v);

        static std::vector<bool> readBoolArray(const Value& v);
        static std::vector<int32_t> readIntArray(const Value& v);
        static std::vector<uint32_t> readUIntArray(const Value& v);
        static std::vector<float> readFloatArray(const Value& v);
        static std::vector<std::string> readStringArray(const Value& v);
        static std::vector<File> readObjectArray(const Value& v);

        static size_t arraySize(const Value& v);
        static Value arrayElement(const Value& v, size_t index);

#if defined(TDF_MODE_CE)
        static void readValue(::VirtualFile* file, Value& v);
#endif

        static bool isArray(Type t);
        static Type elementType(Type arrType);
        static size_t elementSize(Type arrType, Type elemType);
    };
}

using TDFType = CE::TDF::Type;
using TDFValue = CE::TDF::Value;
using TDFFile = CE::TDF::File;
