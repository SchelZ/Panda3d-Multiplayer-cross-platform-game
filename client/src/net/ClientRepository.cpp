#include "net/ClientRepository.h"

#include "core/Log.h"

#include "clockObject.h"
#include "datagramIterator.h"
#include "thread.h"
#include "urlSpec.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>

namespace arena::net {

// Runs the blocking CConnectionRepository::try_connect_net() (DNS + TCP
// connect, bounded by game-server-timeout-ms) off the main thread, so the
// window keeps rendering while we connect.
class ConnectThread final : public Thread {
public:
    ConnectThread(CConnectionRepository *conn, const URLSpec &url)
        : Thread("arena-connect", "arena-connect"), conn_(conn), url_(url) {}

    int result() const { return result_.load(std::memory_order_acquire); }  // -1 = running

protected:
    void thread_main() override {
        const bool ok = conn_->try_connect_net(url_);
        result_.store(ok ? 1 : 0, std::memory_order_release);
    }

private:
    CConnectionRepository *conn_;
    URLSpec url_;
    std::atomic<int> result_{-1};
};

namespace {

double now() { return ClockObject::get_global_clock()->get_real_time(); }

uint16_t readU16(const char *p) {
    const auto *b = reinterpret_cast<const unsigned char *>(p);
    return static_cast<uint16_t>(b[0] | (b[1] << 8));  // DC data is little-endian
}

} // namespace

ClientRepository::ClientRepository() = default;

ClientRepository::~ClientRepository() { disconnect(); }

bool ClientRepository::loadDcFiles(const std::vector<Filename> &files, std::string &error) {
    dcFile_.clear();
    for (const Filename &file : files) {
        if (!dcFile_.read(file)) {
            error = "cannot read DC file " + file.to_os_specific();
            return false;
        }
    }
    if (!dcFile_.all_objects_valid()) {
        error = "DC files reference undefined classes";
        return false;
    }
    dcHash_ = static_cast<uint32_t>(dcFile_.get_hash());
    return true;
}

// ------------------------------------------------------------------ connection

void ClientRepository::connect(const std::string &host, uint16_t port, const std::string &clientInfo) {
    disconnect();
    clientInfo_ = clientInfo;
    target_ = host + ":" + std::to_string(port);
    rejected_ = false;

    // A fresh CConnectionRepository per attempt: no stale queued data.
    // threaded_net = false: poll() reads the socket itself once per frame.
    // Panda's threaded reader can abort the process if a connection is
    // closed while its thread is polling it (Socket_fdset assertion).
    conn_ = std::make_unique<CConnectionRepository>(false, false);
    conn_->set_handle_datagrams_internally(false);  // everything comes to us
    conn_->set_client_datagram(true);

    URLSpec url;
    url.set_scheme("tcp");
    url.set_server(host);
    url.set_port(port);

    LOG_INFO("connecting to %s", target_.c_str());
    state_ = State::Connecting;
    connectThread_ = new ConnectThread(conn_.get(), url);
    if (!Thread::is_threading_supported() || !connectThread_->start(TP_normal, true)) {
        // No threads available: connect synchronously.
        connectThread_ = nullptr;
        if (conn_->try_connect_net(url)) {
            beginSession();
        } else {
            conn_.reset();
            state_ = State::Idle;
            if (onConnectFailed) onConnectFailed("cannot reach " + target_);
        }
    }
}

void ClientRepository::beginSession() {
    state_ = State::Handshake;
    lastReceive_ = now();
    doIdBase_ = doIdRange_ = nextDoId_ = 0;
    freeDoIds_.clear();
    sendHello();
    flush();
}

void ClientRepository::sendHello() {
    Datagram dg;
    dg.add_uint16(CLIENT_HELLO_ARENA);
    dg.add_uint32(kProtocolVersion);
    dg.add_uint32(dcHash_);
    dg.add_uint8(kRolePlayer);
    dg.add_string("");  // token: AI only
    dg.add_string(clientInfo_);
    sendDatagram(dg);
}

void ClientRepository::disconnect() {
    if (connectThread_ != nullptr) {
        connectThread_->join();  // bounded by game-server-timeout-ms
        connectThread_ = nullptr;
    }
    if (conn_ && (state_ == State::Handshake || state_ == State::Ready)) {
        Datagram dg;
        dg.add_uint16(CLIENT_DISCONNECT_CMU);
        sendDatagram(dg);
        conn_->flush();
    }
    endSession(false);
}

void ClientRepository::endSession(bool notifyLost) {
    destroyAllObjects();
    if (conn_) {
        conn_->disconnect();
        conn_.reset();
    }
    const bool wasConnected = state_ == State::Handshake || state_ == State::Ready;
    state_ = State::Idle;
    doIdBase_ = doIdRange_ = nextDoId_ = 0;
    interestZones_.clear();
    clockDelta_.clear();
    if (notifyLost && wasConnected && onLost) {
        onLost();
    }
}

void ClientRepository::poll() {
    if (state_ == State::Connecting) {
        const int result = connectThread_ != nullptr ? connectThread_->result() : 0;
        if (result < 0) {
            return;  // still connecting
        }
        connectThread_->join();
        connectThread_ = nullptr;
        if (result == 0) {
            conn_.reset();
            state_ = State::Idle;
            LOG_WARN("cannot reach %s", target_.c_str());
            if (onConnectFailed) onConnectFailed("cannot reach " + target_);
            return;
        }
        LOG_INFO("connected to %s, sending hello", target_.c_str());
        beginSession();
    }
    if (state_ != State::Handshake && state_ != State::Ready) {
        return;
    }

    // Bounded, so a flood cannot freeze a frame.
    for (int budget = 8192; budget > 0 && conn_ && conn_->check_datagram(); --budget) {
        DatagramIterator di;
        conn_->get_datagram_iterator(di);
        ++stats_.msgsIn;
        stats_.bytesIn += di.get_datagram().get_length();
        lastReceive_ = now();
        handleDatagram(di, conn_->get_msg_type());
    }
    if (!conn_) {
        return;  // a callback disconnected us
    }

    if (!conn_->is_connected()) {
        if (!rejected_) {
            LOG_WARN("lost connection to %s", target_.c_str());
        }
        endSession(!rejected_);
    } else if (now() - lastReceive_ > receiveTimeout) {
        LOG_WARN("no data from %s for %.0f s, dropping the connection", target_.c_str(), receiveTimeout);
        endSession(true);
    }
}

void ClientRepository::flush() {
    if (conn_ && (state_ == State::Handshake || state_ == State::Ready)) {
        conn_->flush();
    }
}

bool ClientRepository::sendDatagram(const Datagram &dg) {
    if (!conn_) {
        return false;
    }
    ++stats_.msgsOut;
    stats_.bytesOut += dg.get_length();
    return conn_->send_datagram(dg);
}

bool ClientRepository::sendPacked(const DCPacker &packer) {
    return sendDatagram(Datagram(packer.get_data(), packer.get_length()));
}

bool ClientRepository::sendMove(const WireState &state) {
    if (state_ != State::Ready) [[unlikely]] {
        return false;
    }
    uint8_t buf[2 + WireState::kSize] = {static_cast<uint8_t>(CLIENT_MOVE & 0xff),
                                        static_cast<uint8_t>(CLIENT_MOVE >> 8)};
    state.encode(buf + 2);
    return sendDatagram(Datagram(buf, sizeof buf));
}

// --------------------------------------------------------------------- objects

uint32_t ClientRepository::ownerOf(uint32_t doId) const {
    if (doIdRange_ == 0 || doId == 0) {
        return 0;
    }
    return ((doId - 1) / doIdRange_) * doIdRange_ + 1;
}

DistributedObject *ClientRepository::find(uint32_t doId) const {
    auto it = objects_.find(doId);
    return it == objects_.end() ? nullptr : it->second.get();
}

DistributedObject *ClientRepository::createLocal(std::unique_ptr<DistributedObject> obj,
                                                 const std::string &className, uint32_t zoneId) {
    const DCClass *dclass = dcFile_.get_class_by_name(className);
    if (state_ != State::Ready || dclass == nullptr || !obj) {
        return nullptr;
    }
    uint32_t doId = 0;
    if (!freeDoIds_.empty()) {
        doId = freeDoIds_.back();
        freeDoIds_.pop_back();
    } else if (nextDoId_ < doIdBase_ + doIdRange_) {
        doId = nextDoId_++;
    } else {
        LOG_ERROR("out of doIds");
        return nullptr;
    }
    obj->doId_ = doId;
    obj->zoneId_ = zoneId;
    obj->ownerId_ = doIdBase_;
    obj->dclass_ = dclass;
    obj->local_ = true;
    DistributedObject *raw = obj.get();
    objects_[doId] = std::move(obj);
    if (!sendGenerate(*raw)) {
        objects_.erase(doId);
        freeDoIds_.push_back(doId);
        return nullptr;
    }
    raw->generated_ = true;
    raw->announceGenerate();
    return raw;
}

void ClientRepository::deleteLocal(uint32_t doId) {
    DistributedObject *obj = find(doId);
    if (obj == nullptr || !obj->isLocal()) {
        return;
    }
    if (state_ == State::Ready) {
        Datagram dg;
        dg.add_uint16(OBJECT_DELETE_CMU);
        dg.add_uint32(doId);
        sendDatagram(dg);
    }
    destroyObject(doId);
    freeDoIds_.push_back(doId);
}

void ClientRepository::setInterestZones(const std::vector<uint32_t> &zones) {
    if (state_ != State::Ready) {
        return;
    }
    Datagram dg;
    dg.add_uint16(CLIENT_SET_INTEREST_CMU);
    for (uint32_t zone : zones) {
        dg.add_uint32(zone);
    }
    sendDatagram(dg);
    interestZones_ = zones;
}

bool ClientRepository::sendGenerate(const DistributedObject &obj) {
    const DCClass &dclass = obj.dclass();
    DCPacker packer;
    packer.raw_pack_uint16(CLIENT_OBJECT_GENERATE_CMU);
    packer.raw_pack_uint32(obj.zoneId());
    packer.raw_pack_uint16(static_cast<unsigned>(dclass.get_number()));
    packer.raw_pack_uint32(obj.doId());

    // Required fields, in inheritance order (DCClass::client_format_generate_CMU).
    const int numFields = dclass.get_num_inherited_fields();
    for (int i = 0; i < numFields; ++i) {
        const DCField *field = dclass.get_inherited_field(i);
        if (!field->is_required() || field->as_molecular_field() != nullptr) {
            continue;
        }
        packer.begin_pack(field);
        packer.push();
        FieldWriter out(packer);
        const bool provided = obj.packField(*field, out);
        packer.pop();
        if (!packer.end_pack() || !provided) {
            LOG_ERROR("cannot pack required field %s.%s", dclass.get_name().c_str(),
                      field->get_name().c_str());
            return false;
        }
    }

    // Plus the broadcast ram fields this object provides (what
    // ClientRepository.resendGenerate() adds), so late joiners see the
    // current state, e.g. the position.
    DCPacker optional;
    unsigned count = 0;
    for (int i = 0; i < numFields; ++i) {
        const DCField *field = dclass.get_inherited_field(i);
        if (field->is_required() || !field->is_broadcast() || !field->is_ram() ||
            field->as_molecular_field() != nullptr) {
            continue;
        }
        DCPacker one;
        one.begin_pack(field);
        one.push();
        FieldWriter out(one);
        const bool provided = obj.packField(*field, out);
        one.pop();
        if (one.end_pack() && provided) {
            optional.raw_pack_uint16(static_cast<unsigned>(field->get_number()));
            optional.append_data(reinterpret_cast<const unsigned char *>(one.get_data()), one.get_length());
            ++count;
        }
    }
    packer.raw_pack_uint16(count);
    packer.append_data(reinterpret_cast<const unsigned char *>(optional.get_data()), optional.get_length());
    return sendPacked(packer);
}

void ClientRepository::destroyObject(uint32_t doId) {
    auto it = objects_.find(doId);
    if (it == objects_.end()) {
        return;
    }
    // Detach first: disable() may look objects up or trigger game callbacks.
    std::unique_ptr<DistributedObject> obj = std::move(it->second);
    objects_.erase(it);
    try {
        obj->disable();
    } catch (const std::exception &e) {
        LOG_ERROR("disable() of %u failed: %s", doId, e.what());
    }
}

void ClientRepository::destroyAllObjects() {
    std::vector<uint32_t> ids;
    ids.reserve(objects_.size());
    for (const auto &kv : objects_) {
        ids.push_back(kv.first);
    }
    for (uint32_t id : ids) {
        destroyObject(id);
    }
    timeManager_ = nullptr;
}

// -------------------------------------------------------------------- receive

void ClientRepository::handleDatagram(DatagramIterator &di, unsigned int msgType) {
    currentSender_ = 0;
    switch (msgType) {
    case SERVER_MOVES: handleMoves(di); break;  // the hot path
    case SET_DOID_RANGE_CMU: handleSetDoIdRange(di); break;
    case OBJECT_GENERATE_CMU: handleGenerate(di); break;
    case OBJECT_UPDATE_FIELD_CMU: handleUpdate(di); break;
    case OBJECT_DISABLE_CMU: handleDisable(di); break;
    case OBJECT_DELETE_CMU: handleDelete(di); break;
    case REQUEST_GENERATES_CMU: handleRequestGenerates(di); break;
    case SERVER_REJECT_ARENA: handleReject(di); break;
    case SERVER_SLOTS: handleSlots(di); break;
    default:
        ++stats_.dropped;
        LOG_WARN("unexpected message type %u", msgType);
        break;
    }
}

void ClientRepository::handleSetDoIdRange(DatagramIterator &di) {
    if (di.get_remaining_size() != 8 || state_ != State::Handshake) {
        ++stats_.dropped;
        return;
    }
    const uint32_t base = di.get_uint32();
    const uint32_t range = di.get_uint32();
    if (base == 0 || range == 0) {
        ++stats_.dropped;
        return;
    }
    doIdBase_ = base;
    doIdRange_ = range;
    nextDoId_ = base;
    state_ = State::Ready;
    LOG_INFO("joined %s as client %u", target_.c_str(), base);
    if (onReady) onReady();
}

size_t ClientRepository::validateField(const DCField &field, const char *data, size_t size) {
    DCPacker packer;
    packer.set_unpack_data(data, size, false);
    packer.begin_unpack(&field);
    packer.unpack_validate();
    const bool ok = packer.end_unpack();
    const size_t used = packer.get_num_unpacked_bytes();
    return ok && used > 0 && used <= size ? used : 0;
}

bool ClientRepository::parseGenerate(const DCClass &dclass, const char *data, size_t size,
                                     std::vector<FieldSlice> &out) const {
    size_t off = 0;
    const int numFields = dclass.get_num_inherited_fields();
    for (int i = 0; i < numFields; ++i) {
        const DCField *field = dclass.get_inherited_field(i);
        if (!field->is_required() || !field->is_broadcast() || field->as_molecular_field() != nullptr) {
            continue;
        }
        const size_t used = validateField(*field, data + off, size - off);
        if (used == 0) {
            return false;
        }
        out.push_back({field, off, used});
        off += used;
    }
    if (size - off < 2) {
        return false;
    }
    const unsigned count = readU16(data + off);
    off += 2;
    if (count > 256) {
        return false;
    }
    for (unsigned k = 0; k < count; ++k) {
        if (size - off < 2) {
            return false;
        }
        const DCField *field = dclass.get_field_by_index(readU16(data + off));
        off += 2;
        if (field == nullptr) {
            return false;
        }
        const size_t used = validateField(*field, data + off, size - off);
        if (used == 0) {
            return false;
        }
        out.push_back({field, off, used});
        off += used;
    }
    return off == size;
}

void ClientRepository::dispatch(DistributedObject &obj, const FieldSlice &slice, const char *base) {
    DCPacker packer;
    packer.set_unpack_data(base + slice.offset, slice.size, false);
    packer.begin_unpack(slice.field);
    packer.push();
    FieldReader in(packer);
    try {
        obj.receiveField(*slice.field, in);
    } catch (const std::exception &e) {
        LOG_ERROR("%s.%s handler failed: %s", obj.dclass().get_name().c_str(),
                  slice.field->get_name().c_str(), e.what());
    }
    packer.pop();
    packer.end_unpack();  // a handler may skip fields it does not care about
}

void ClientRepository::handleGenerate(DatagramIterator &di) {
    if (di.get_remaining_size() < 14) {
        ++stats_.dropped;
        return;
    }
    const uint32_t sender = di.get_uint32();
    const uint32_t zoneId = di.get_uint32();
    const unsigned classId = di.get_uint16();
    const uint32_t doId = di.get_uint32();
    currentSender_ = sender;

    const DCClass *dclass = classId < static_cast<unsigned>(dcFile_.get_num_classes())
                                ? dcFile_.get_class(static_cast<int>(classId))
                                : nullptr;
    if (dclass == nullptr || dclass->is_struct() || dclass->get_number() != static_cast<int>(classId) ||
        sender != ownerOf(doId) || sender == doIdBase_) {
        ++stats_.dropped;
        return;
    }

    const char *data = static_cast<const char *>(di.get_datagram().get_data()) + di.get_current_index();
    const size_t size = di.get_remaining_size();
    std::vector<FieldSlice> slices;
    if (!parseGenerate(*dclass, data, size, slices)) {
        ++stats_.dropped;
        LOG_WARN("malformed generate for %s %u", dclass->get_name().c_str(), doId);
        return;
    }

    if (DistributedObject *existing = find(doId)) {
        // A repeat generate (someone else entered the zone): just refresh.
        if (&existing->dclass() != dclass || existing->isLocal()) {
            ++stats_.dropped;
            return;
        }
        existing->zoneId_ = zoneId;
        for (const FieldSlice &slice : slices) {
            dispatch(*existing, slice, data);
        }
        return;
    }

    auto factory = factories_.find(dclass->get_number());
    if (factory == factories_.end()) {
        return;  // a class this client does not display
    }
    std::unique_ptr<DistributedObject> obj = factory->second(*this);
    if (!obj) {
        return;
    }
    obj->doId_ = doId;
    obj->zoneId_ = zoneId;
    obj->ownerId_ = sender;
    obj->dclass_ = dclass;
    DistributedObject *raw = obj.get();
    objects_[doId] = std::move(obj);
    for (const FieldSlice &slice : slices) {
        dispatch(*raw, slice, data);
    }
    raw->generated_ = true;
    raw->announceGenerate();
}

void ClientRepository::handleUpdate(DatagramIterator &di) {
    if (di.get_remaining_size() < 10) {
        ++stats_.dropped;
        return;
    }
    const uint32_t sender = di.get_uint32();
    const uint32_t doId = di.get_uint32();
    const unsigned fieldId = di.get_uint16();
    currentSender_ = sender;

    DistributedObject *obj = find(doId);
    if (obj == nullptr) {
        return;  // not (or no longer) visible to us
    }
    // Every update this client legitimately receives comes from the object's
    // owner (broadcasts from players, targeted replies from the AI).
    if (sender != obj->ownerId()) {
        ++stats_.dropped;
        return;
    }
    const DCField *field = obj->dclass().get_field_by_index(static_cast<int>(fieldId));
    const char *data = static_cast<const char *>(di.get_datagram().get_data()) + di.get_current_index();
    const size_t size = di.get_remaining_size();
    if (field == nullptr || size == 0 || validateField(*field, data, size) != size) {
        ++stats_.dropped;
        return;
    }
    dispatch(*obj, FieldSlice{field, 0, size}, data);
}

void ClientRepository::handleDisable(DatagramIterator &di) {
    if (di.get_remaining_size() % 4 != 0) {
        ++stats_.dropped;
        return;
    }
    while (di.get_remaining_size() > 0) {
        const uint32_t doId = di.get_uint32();
        DistributedObject *obj = find(doId);
        if (obj != nullptr && !obj->isLocal()) {
            destroyObject(doId);
        }
    }
}

void ClientRepository::handleDelete(DatagramIterator &di) {
    if (di.get_remaining_size() != 4) {
        ++stats_.dropped;
        return;
    }
    const uint32_t doId = di.get_uint32();
    DistributedObject *obj = find(doId);
    if (obj != nullptr && !obj->isLocal()) {
        destroyObject(doId);
    }
}

void ClientRepository::handleRequestGenerates(DatagramIterator &di) {
    if (di.get_remaining_size() != 4) {
        ++stats_.dropped;
        return;
    }
    const uint32_t zoneId = di.get_uint32();
    for (const auto &kv : objects_) {
        const DistributedObject &obj = *kv.second;
        if (obj.isLocal() && obj.generated() && obj.zoneId() == zoneId) {
            sendGenerate(obj);
        }
    }
}

// uint8 count, count x (uint8 slot, 9-byte WireState). Checked as a whole first.
void ClientRepository::handleMoves(DatagramIterator &di) {
    const auto *data = static_cast<const uint8_t *>(di.get_datagram().get_data()) + di.get_current_index();
    const size_t size = di.get_remaining_size();
    constexpr size_t kEntry = 1 + WireState::kSize;
    if (size == 0 || size != 1 + data[0] * kEntry || state_ != State::Ready) [[unlikely]] {
        ++stats_.dropped;
        return;
    }
    for (const uint8_t *e = data + 1, *end = data + size; e < end; e += kEntry) {
        const WireState s = WireState::decode(e + 1);
        if (e[0] == kNoSlot) [[unlikely]] {
            ++stats_.dropped;
            continue;
        }
        if (onMove) onMove(e[0], s);
    }
}

// k x (uint8 slot, uint32 doId).
void ClientRepository::handleSlots(DatagramIterator &di) {
    const size_t size = di.get_remaining_size();
    if (size == 0 || size % 5 != 0) [[unlikely]] {
        ++stats_.dropped;
        return;
    }
    while (di.get_remaining_size() > 0) {
        const uint8_t slot = di.get_uint8();
        const uint32_t doId = di.get_uint32();
        if (slot != kNoSlot && onSlot) onSlot(slot, doId);
    }
}

void ClientRepository::handleReject(DatagramIterator &di) {
    uint16_t code = 0;
    std::string reason = "rejected";
    if (di.get_remaining_size() >= 4) {
        code = di.get_uint16();
        const size_t len = readU16(static_cast<const char *>(di.get_datagram().get_data()) + di.get_current_index());
        if (di.get_remaining_size() >= 2 + len) {
            reason = di.get_string();
        }
    }
    rejected_ = true;
    LOG_WARN("server refused us: %s (code %u)", reason.c_str(), code);
    if (onRejected) onRejected(code, reason);
}

} // namespace arena::net
