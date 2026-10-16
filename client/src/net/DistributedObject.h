// Base class for C++ views of distributed objects (the C++ counterpart of
// direct.distributed.DistributedObject).
#pragma once

#include "net/FieldIO.h"

#include "dcClass.h"
#include "dcField.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace arena::net {

class ClientRepository;

class DistributedObject {
public:
    explicit DistributedObject(ClientRepository &cr) : cr_(cr) {}
    virtual ~DistributedObject() = default;
    DistributedObject(const DistributedObject &) = delete;
    DistributedObject &operator=(const DistributedObject &) = delete;

    uint32_t doId() const { return doId_; }
    uint32_t zoneId() const { return zoneId_; }
    uint32_t ownerId() const { return ownerId_; }  // doIdBase of the owning client
    const DCClass &dclass() const { return *dclass_; }
    bool isLocal() const { return local_; }        // created by this client
    bool generated() const { return generated_; }

    // Field values arriving from the network (generate or update). Unknown
    // fields are ignored. The data was validated against the DC file.
    virtual void receiveField(const DCField &field, FieldReader &in) = 0;
    // Field values for generate messages of local objects. Return false if
    // this object does not provide the field.
    virtual bool packField(const DCField &field, FieldWriter &out) const = 0;

    // All initial fields applied; the object is now visible to the game.
    virtual void announceGenerate() {}
    // Leaving the game (deleted, out of interest, or disconnected). The
    // repository destroys the object right after.
    virtual void disable() {}

protected:
    ClientRepository &cr_;

private:
    friend class ClientRepository;
    uint32_t doId_ = 0;
    uint32_t zoneId_ = 0;
    uint32_t ownerId_ = 0;
    const DCClass *dclass_ = nullptr;
    bool local_ = false;
    bool generated_ = false;
};

// Maps DC field numbers to member functions, so dispatch is one vector index
// instead of string compares. One static table per class, bound at startup;
// binding fails loudly if the DC file lacks a field the code expects.
template <class T>
class FieldTable {
public:
    using Receiver = void (T::*)(FieldReader &);
    using Packer = void (T::*)(FieldWriter &) const;

    bool bind(const DCClass &dclass, const char *name, Receiver recv, Packer pack = nullptr) {
        const DCField *field = dclass.get_field_by_name(name);
        if (field == nullptr) {
            missing_.emplace_back(dclass.get_name() + "." + name);
            return false;
        }
        const size_t n = static_cast<size_t>(field->get_number());
        if (recv_.size() <= n) {
            recv_.resize(n + 1, nullptr);
            pack_.resize(n + 1, nullptr);
        }
        recv_[n] = recv;
        pack_[n] = pack;
        return true;
    }

    const DCField *field(const DCClass &dclass, const char *name) {
        const DCField *f = dclass.get_field_by_name(name);
        if (f == nullptr) {
            missing_.emplace_back(dclass.get_name() + "." + name);
        }
        return f;
    }

    bool receive(T &obj, const DCField &field, FieldReader &in) const {
        const size_t n = static_cast<size_t>(field.get_number());
        if (n >= recv_.size() || recv_[n] == nullptr) {
            return false;
        }
        (obj.*recv_[n])(in);
        return true;
    }

    bool pack(const T &obj, const DCField &field, FieldWriter &out) const {
        const size_t n = static_cast<size_t>(field.get_number());
        if (n >= pack_.size() || pack_[n] == nullptr) {
            return false;
        }
        (obj.*pack_[n])(out);
        return true;
    }

    const std::vector<std::string> &missing() const { return missing_; }

private:
    std::vector<Receiver> recv_;
    std::vector<Packer> pack_;
    std::vector<std::string> missing_;
};

} // namespace arena::net
