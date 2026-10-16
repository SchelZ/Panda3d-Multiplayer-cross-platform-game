// C++ client for Panda3D's distributed networking (CMU ServerRepository
// protocol), built on CConnectionRepository + DCFile/DCPacker from
// libp3direct. It mirrors direct/distributed/ClientRepository.py:
//
//   connect -> hello -> SET_DOID_RANGE -> createReady
//   setInterestZones() -> OBJECT_GENERATE / UPDATE_FIELD / DISABLE / DELETE
//   REQUEST_GENERATES -> resend our objects for late joiners
//   SERVER_SLOTS / SERVER_MOVES / CLIENT_MOVE -> compact binary movement
//
// and adds what a shipped game needs: non-blocking connect on a worker
// thread, a version/DC-hash handshake, payload validation before any field
// reaches game code, sender checks, a receive watchdog and statistics.
#pragma once

#include "net/ClockDelta.h"
#include "net/DistributedObject.h"
#include "net/Protocol.h"

#include "cConnectionRepository.h"
#include "datagram.h"
#include "dcFile.h"
#include "filename.h"
#include "pointerTo.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace arena::net {

class ConnectThread;
class TimeManager;

class ClientRepository {
public:
    enum class State { Idle, Connecting, Handshake, Ready };

    struct Stats {
        uint64_t msgsIn = 0, msgsOut = 0, bytesIn = 0, bytesOut = 0;
        uint64_t dropped = 0;  // malformed, unknown or not from the owner
    };

    using Factory = std::function<std::unique_ptr<DistributedObject>(ClientRepository &)>;

    ClientRepository();
    ~ClientRepository();
    ClientRepository(const ClientRepository &) = delete;
    ClientRepository &operator=(const ClientRepository &) = delete;

    // Reads the DC files (same files, same order as the server).
    bool loadDcFiles(const std::vector<Filename> &files, std::string &error);
    const DCFile &dcFile() const { return dcFile_; }
    uint32_t dcHash() const { return dcHash_; }

    // T must provide `static bool bindFields(const DCClass &)`.
    template <class T>
    bool registerClass(const std::string &name, Factory factory, std::string &error);

    // --- connection -------------------------------------------------------
    void connect(const std::string &host, uint16_t port, const std::string &clientInfo);
    void disconnect();  // graceful; destroys all objects
    State state() const { return state_; }
    bool isReady() const { return state_ == State::Ready; }

    // Pump the network: completes connects, dispatches every queued message
    // (bounded per call), detects lost connections. Call once per frame.
    void poll();
    // Send everything queued this frame (collect-tcp batching).
    void flush();

    // --- objects ------------------------------------------------------------
    uint32_t doIdBase() const { return doIdBase_; }
    uint32_t ownerOf(uint32_t doId) const;
    DistributedObject *find(uint32_t doId) const;
    template <class T> T *findAs(uint32_t doId) const { return dynamic_cast<T *>(find(doId)); }

    // Creates a local object owned by this client and announces it to the
    // zone (generate with required + ram fields). Returns null on failure.
    DistributedObject *createLocal(std::unique_ptr<DistributedObject> obj, const std::string &className,
                                   uint32_t zoneId);
    void deleteLocal(uint32_t doId);
    void setInterestZones(const std::vector<uint32_t> &zones);

    // Sends one field update. `args` writes the field's parameters.
    template <class Fn>
    bool sendUpdate(const DistributedObject &obj, const DCField *field, Fn &&args);

    // Sends our player's movement sample (CLIENT_MOVE, 13 bytes on the wire).
    bool sendMove(const WireState &state);

    // doIdBase of the client that sent the message being dispatched.
    uint32_t currentSender() const { return currentSender_; }

    ClockDelta &clockDelta() { return clockDelta_; }
    TimeManager *timeManager() const { return timeManager_; }
    void setTimeManager(TimeManager *tm) { timeManager_ = tm; }

    const Stats &stats() const { return stats_; }
    double lastReceiveTime() const { return lastReceive_; }

    // --- callbacks (all invoked from poll()) ---------------------------------
    std::function<void()> onReady;                             // doId range received
    std::function<void(const std::string &)> onConnectFailed;  // never reached the server
    std::function<void(uint16_t, const std::string &)> onRejected;
    std::function<void()> onLost;                              // connection dropped
    std::function<void(uint8_t slot, uint32_t doId)> onSlot;   // doId 0: slot freed
    std::function<void(uint8_t slot, const WireState &)> onMove;

    // A connection that delivers nothing for this long is treated as dead
    // (the game pings the AI every 2 s, so silence means a half-open socket).
    double receiveTimeout = 15.0;

private:
    struct FieldSlice {
        const DCField *field;
        size_t offset;
        size_t size;
    };

    void beginSession();
    void endSession(bool notifyLost);
    void sendHello();
    bool sendDatagram(const Datagram &dg);
    bool sendPacked(const DCPacker &packer);
    bool sendGenerate(const DistributedObject &obj);
    void destroyAllObjects();
    void destroyObject(uint32_t doId);

    void handleDatagram(DatagramIterator &di, unsigned int msgType);
    void handleSetDoIdRange(DatagramIterator &di);
    void handleGenerate(DatagramIterator &di);
    void handleUpdate(DatagramIterator &di);
    void handleDisable(DatagramIterator &di);
    void handleDelete(DatagramIterator &di);
    void handleRequestGenerates(DatagramIterator &di);
    void handleReject(DatagramIterator &di);
    void handleSlots(DatagramIterator &di);
    void handleMoves(DatagramIterator &di);

    static size_t validateField(const DCField &field, const char *data, size_t size);
    bool parseGenerate(const DCClass &dclass, const char *data, size_t size,
                       std::vector<FieldSlice> &out) const;
    void dispatch(DistributedObject &obj, const FieldSlice &slice, const char *base);

    DCFile dcFile_;
    uint32_t dcHash_ = 0;
    std::unordered_map<int, Factory> factories_;  // by class number

    std::unique_ptr<CConnectionRepository> conn_;
    PT(ConnectThread) connectThread_;
    State state_ = State::Idle;
    std::string clientInfo_;
    std::string target_;
    bool rejected_ = false;

    uint32_t doIdBase_ = 0;
    uint32_t doIdRange_ = 0;
    uint32_t nextDoId_ = 0;
    std::vector<uint32_t> freeDoIds_;
    uint32_t currentSender_ = 0;

    std::unordered_map<uint32_t, std::unique_ptr<DistributedObject>> objects_;
    std::vector<uint32_t> interestZones_;
    TimeManager *timeManager_ = nullptr;
    ClockDelta clockDelta_;
    Stats stats_;
    double lastReceive_ = 0.0;
};

// --------------------------------------------------------------------------

template <class T>
bool ClientRepository::registerClass(const std::string &name, Factory factory, std::string &error) {
    const DCClass *dclass = dcFile_.get_class_by_name(name);
    if (dclass == nullptr || dclass->is_struct()) {
        error = "DC files define no class " + name;
        return false;
    }
    if (!T::bindFields(*dclass)) {
        error = "DC class " + name + " lacks fields the client needs";
        return false;
    }
    factories_[dclass->get_number()] = std::move(factory);
    return true;
}

template <class Fn>
bool ClientRepository::sendUpdate(const DistributedObject &obj, const DCField *field, Fn &&args) {
    if (state_ != State::Ready || field == nullptr || !obj.generated()) {
        return false;
    }
    DCPacker packer;
    packer.raw_pack_uint16(CLIENT_OBJECT_UPDATE_FIELD);
    packer.raw_pack_uint32(obj.doId());
    packer.raw_pack_uint16(static_cast<unsigned>(field->get_number()));
    packer.begin_pack(field);
    packer.push();
    FieldWriter out(packer);
    args(out);
    packer.pop();
    if (!packer.end_pack()) {
        ++stats_.dropped;
        return false;
    }
    return sendPacked(packer);
}

} // namespace arena::net
