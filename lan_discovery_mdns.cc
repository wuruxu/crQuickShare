// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/lan_discovery.h"
#include "crQuickShare/mdns_codec.h"
#include "crQuickShare/mdns_network.h"
#include <algorithm>
#include <map>
#include <iterator>
#include <set>
#include <tuple>
#include <utility>
#include "base/base64url.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
namespace crquickshare {
MdnsNetwork::~MdnsNetwork() = default;
namespace {
constexpr char kService[] = "_FC9F5ED42C8A._tcp.local";
constexpr std::string_view kHash("\xfc\x9f\x5e", 3);
using mdns::Canonical;
using mdns::Record;
using mdns::Packet;
using mdns::U16;
using mdns::U32;
using mdns::Get16;
using mdns::Name;
std::string B64(std::string_view value) {
  std::string out; base::Base64UrlEncode(value, base::Base64UrlEncodePolicy::OMIT_PADDING, &out); return out;
}
std::string Decode(std::string_view value) {
  std::string out;
  if (!base::Base64UrlDecode(value, base::Base64UrlDecodePolicy::IGNORE_PADDING, &out)) return {};
  return out;
}
std::string Endpoint(std::string_view instance) {
  const auto raw = Decode(instance.substr(0, instance.find('.')));
  return raw.size() >= 9 && uint8_t(raw[0]) == 0x23 && raw.substr(5, 3) == kHash ? raw.substr(1, 4) : std::string();
}
std::string EndpointInfo(const Record& record) {
  for (size_t at = 0; at < record.data.size();) {
    const size_t length = uint8_t(record.data[at++]);
    if (length > record.data.size() - at) return {};
    const auto item = std::string_view(record.data).substr(at, length); at += length;
    if (item.starts_with("n=")) return Decode(item.substr(2));
  }
  return {};
}
std::string AddressText(uint32_t address) {
  return base::NumberToString(address >> 24) + "." + base::NumberToString((address >> 16) & 255) + "." +
      base::NumberToString((address >> 8) & 255) + "." + base::NumberToString(address & 255);
}
const char* EndpointNameStatus(const std::string& info) {
  const char* reason = nullptr;
  auto parsed = ParseEndpointInfo(info, reason);
  if (!parsed) return reason;
  return parsed->name ? "available" : "not_public";
}
std::optional<uint32_t> ParseAddress(std::string_view text) {
  const auto parts = base::SplitStringPiece(text, ".", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  if (parts.size() != 4) return std::nullopt;
  uint32_t address = 0;
  for (auto part : parts) {
    unsigned value;
    if (part.empty() || part.size() > 3 || !std::ranges::all_of(part, [](char c) { return c >= '0' && c <= '9'; }) ||
        !base::StringToUint(part, &value) || value > 255) return std::nullopt;
    address = (address << 8) | value;
  }
  return address;
}
bool Matches(const mdns::Question& question, const Record& record) {
  return (question.klass & 0x7fff) == 1 && Canonical(question.name) == Canonical(record.name) &&
      (question.type == 255 || question.type == record.type);
}
void AddUnique(std::vector<Record>& records, const Record& record) {
  if (std::ranges::none_of(records, [&](const auto& existing) { return mdns::Same(existing, record); })) records.push_back(record);
}
struct Lifetime {
  base::TimeTicks received, until, refresh;
  uint32_t ttl = 0;
  int attempt = 0;
  bool withdrawing = false;
  const char* removal = "ttl_expired";
  bool Valid(base::TimeTicks now) const { return until > now; }
  void Schedule() { refresh = received + base::Seconds(ttl) * ((80 + attempt * 5 + base::RandDouble() * 2) / 100.0); }
  void Update(uint32_t seconds, base::TimeTicks now) {
    ttl = seconds; received = now; until = now + base::Seconds(seconds); attempt = 0; withdrawing = false; removal = "ttl_expired"; Schedule();
  }
  void Withdraw(base::TimeTicks now, const char* reason) {
    if (!withdrawing && Valid(now)) removal = reason;
    until = std::min(until, now + base::Seconds(1)); withdrawing = true; attempt = 4;
  }
  bool RefreshDue(base::TimeTicks now) {
    if (!Valid(now) || attempt >= 4 || now < refresh) return false;
    do { ++attempt; if (attempt < 4) Schedule(); } while (attempt < 4 && refresh <= now);
    return true;
  }
};
struct Cached { Record record; Lifetime life; uint64_t generation = 0; };
using CacheKey = std::tuple<unsigned, std::string, uint16_t>;
CacheKey Key(unsigned index, std::string_view name, uint16_t type) { return {index, Canonical(name), type}; }
struct Publication {
  unsigned probes = 0, announcements = 0;
  bool established = false;
  base::TimeTicks due, last_multicast;
};
struct PendingReply {
  MdnsDestination source;
  unsigned index = 0;
  bool unicast = false, legacy = false;
  uint16_t id = 0;
  base::TimeTicks due, limit;
  std::vector<mdns::Question> questions;
  std::vector<Record> answers, additional;
};
class MdnsDiscovery final : public LanDiscovery {
 public:
  MdnsDiscovery(const std::string& endpoint, const std::string& info, uint16_t port);
  ~MdnsDiscovery() override;
  bool ready() const override;
  void Tick(bool visible) override;
  void RestartQueries() override;
  void RefreshAdvertisement() override;
  std::vector<Peer> Peers() const override;
  bool IsLocalAddress(const std::string& address) const override;
  std::string local_endpoint_id() const override;
 private:
  bool Local(uint32_t address, unsigned index = 0) const;
  void RefreshNetwork(base::TimeTicks now);
  void ResetPublications(base::TimeTicks now, base::TimeDelta delay = base::TimeDelta());
  void SetNames();
  std::vector<Record> OwnRecords(unsigned index) const;
  bool Send(const Packet& packet, unsigned index, const MdnsDestination* destination = nullptr);
  bool Announce(unsigned index, uint32_t ttl);
  void Publish(base::TimeTicks now);
  bool Conflict(const Packet& packet, unsigned index, base::TimeTicks now);
  void Rename(bool instance_conflict, base::TimeTicks now);
  void Read(const MdnsDatagram& datagram, base::TimeTicks now);
  void QueueReply(const Packet& packet, const MdnsDatagram& datagram, base::TimeTicks now);
  void FlushReplies(base::TimeTicks now);
  void Cache(const std::vector<Record>& records, unsigned index, base::TimeTicks now);
  const Cached* Select(unsigned index, std::string_view name, uint16_t type, base::TimeTicks now) const;
  void Query(std::string_view name, uint16_t type, unsigned index, base::TimeTicks now);
  void MaintainCache(base::TimeTicks now);
  void DiagnoseVisibility(base::TimeTicks now);
  bool AllowVisibilityLog(base::TimeTicks now);
  std::unique_ptr<MdnsNetwork> network_ = CreateMdnsNetwork();
  std::vector<MdnsInterface> interfaces_;
  std::map<unsigned, Publication> publications_;
  std::map<CacheKey, std::vector<Cached>> cache_;
  std::map<CacheKey, base::TimeTicks> recent_queries_;
  struct Resolution { base::TimeTicks due; int interval = 1; uint64_t id = 0; };
  std::map<CacheKey, Resolution> resolutions_;
  // Diagnostics only. Keep the last visible SRV target so target changes can
  // be distinguished from expiry of the previous address. Never log the name.
  struct ObservedService { uint64_t id = 0; std::string target; };
  std::map<CacheKey, ObservedService> observed_services_;
  base::TimeTicks visibility_log_window_;
  unsigned visibility_logs_ = 0;
  uint64_t visibility_logs_suppressed_ = 0;
  uint64_t unchanged_network_notifications_ = 0;
  base::TimeTicks next_notification_refresh_;
  std::vector<PendingReply> replies_;
  std::string endpoint_, info_, instance_, host_;
  uint16_t port_;
  bool visible_ = false;
  base::TimeTicks next_network_check_, next_query_, next_diagnostics_, conflict_window_;
  base::Time last_network_tick_;
  int query_interval_ = 1;
  unsigned conflicts_ = 0;
  uint64_t generation_ = 0, next_id_ = 0;
  uint64_t received_ = 0, malformed_ = 0, queries_ = 0, suppressed_ = 0, announcements_ = 0, send_errors_ = 0, receive_errors_ = 0;
};
MdnsDiscovery::MdnsDiscovery(const std::string& endpoint, const std::string& info, uint16_t port)
    : endpoint_(endpoint), info_(info), port_(port) {
  SetNames(); RefreshNetwork(base::TimeTicks::Now());
}
MdnsDiscovery::~MdnsDiscovery() {
  for (const auto& [index, publication] : publications_) if (publication.established) Announce(index, 0);
}
void MdnsDiscovery::SetNames() {
  instance_ = B64(std::string(1, '\x23') + endpoint_ + std::string(kHash) + std::string(2, '\0')) + "." + kService;
  host_ = "crquickshare-" + endpoint_ + ".local";
}
bool MdnsDiscovery::ready() const { return network_->ready(); }
std::string MdnsDiscovery::local_endpoint_id() const { return endpoint_; }
bool MdnsDiscovery::Local(uint32_t address, unsigned index) const {
  if (!address || address == 0xffffffff || (address >> 24) == 127 || (address >> 28) >= 14) return false;
  return std::ranges::any_of(interfaces_, [&](const auto& entry) {
    return (!index || index == entry.index) && (address & entry.mask) == (entry.address & entry.mask);
  });
}
bool MdnsDiscovery::IsLocalAddress(const std::string& address) const {
  const auto parsed = ParseAddress(address); return parsed && Local(*parsed);
}
void MdnsDiscovery::ResetPublications(base::TimeTicks now, base::TimeDelta delay) {
  replies_.clear(); publications_.clear();
  if (!visible_) return;
  for (const auto& entry : interfaces_) {
    auto& publication = publications_[entry.index];
    publication.due = now + delay + base::Milliseconds(base::RandIntInclusive(0, 250));
  }
}
void MdnsDiscovery::RefreshNetwork(base::TimeTicks now) {
  const bool notified = network_->Changed();
  const auto wall = base::Time::Now();
  const bool resumed = !last_network_tick_.is_null() && wall - last_network_tick_ > base::Seconds(10);
  last_network_tick_ = wall;
  if (!notified && !resumed && now < next_network_check_) return;
  next_network_check_ = now + base::Seconds(3);
  auto current = network_->Interfaces();
  if (!current) return;
  if (*current == interfaces_ && !resumed && (ready() || current->empty())) {
    if (notified) {
      // A route notification need not change the multicast interfaces.
      ++unchanged_network_notifications_;
      if (now >= next_notification_refresh_) {
        next_notification_refresh_ = now + base::Seconds(1);
        next_query_ = {}; query_interval_ = 1;
        RefreshAdvertisement();
      }
    }
    return;
  }
  std::set<unsigned> affected;
  for (const auto& link : interfaces_)
    if (std::ranges::find(*current, link) == current->end()) affected.insert(link.index);
  for (const auto& link : *current)
    if (std::ranges::find(interfaces_, link) == interfaces_.end()) affected.insert(link.index);
  const bool invalidate_all = resumed || !ready();
  const char* reset_reason = resumed ? "network_resume" : *current != interfaces_ ? "network_changed" :
      notified ? "network_notification" : "network_reopened";
  for (const auto& [key, observed] : observed_services_) if ((invalidate_all || affected.contains(std::get<0>(key))) && AllowVisibilityLog(now))
    LOG(INFO) << "crQuickShare: mDNS service unavailable; cache_id=" << observed.id
              << "; interface=" << std::get<0>(key) << "; record=NETWORK; reason=" << reset_reason;
  auto invalidated = [&](const auto& pair) { return invalidate_all || affected.contains(std::get<0>(pair.first)); };
  std::erase_if(observed_services_, invalidated);
  for (const auto& [index, publication] : publications_)
    if (publication.established && (invalidate_all || affected.contains(index))) Announce(index, 0);
  auto previous_publications = publications_;
  network_->Close(); interfaces_ = std::move(*current);
  std::erase_if(cache_, invalidated);
  std::erase_if(resolutions_, invalidated);
  std::erase_if(recent_queries_, invalidated);
  network_->Open(interfaces_); ResetPublications(now);
  if (ready() && !invalidate_all) for (const auto& [index, publication] : previous_publications) {
    if (!affected.contains(index) && publications_.contains(index)) publications_[index] = publication;
  }
  next_query_ = {}; query_interval_ = 1;
  LOG(INFO) << "crQuickShare: Network discovery reset; ready=" << ready() << "; interface_addresses=" << interfaces_.size()
            << "; resumed=" << resumed << "; notified=" << notified
            << "; invalidated_all=" << invalidate_all << "; affected_interfaces=" << affected.size();
  for (const auto& link : interfaces_)
    LOG(INFO) << "crQuickShare: mDNS interface; interface=" << link.index
              << "; local_ipv4=" << AddressText(link.address) << "; netmask=" << AddressText(link.mask)
              << "; subnet=" << AddressText(link.address & link.mask);
}
std::vector<Record> MdnsDiscovery::OwnRecords(unsigned index) const {
  std::vector<Record> records;
  records.push_back({kService, 12, 1, 120, Name(instance_), instance_});
  std::string srv; U16(srv, 0); U16(srv, 0); U16(srv, port_); srv += Name(host_);
  records.push_back({instance_, 33, 0x8001, 120, srv, host_});
  const auto txt = "n=" + B64(info_);
  records.push_back({instance_, 16, 0x8001, 120, std::string(1, char(txt.size())) + txt, {}});
  for (const auto& entry : interfaces_) if (entry.index == index) {
    std::string address; U32(address, entry.address);
    AddUnique(records, {host_, 1, 0x8001, 120, address, {}});
  }
  return records;
}
bool MdnsDiscovery::Send(const Packet& packet, unsigned index, const MdnsDestination* destination) {
  const auto interface = std::ranges::find(interfaces_, index, &MdnsInterface::index);
  if (interface == interfaces_.end()) return false;
  const auto bytes = mdns::Encode(packet);
  if (bytes.empty() || !network_->Send(bytes, *interface, destination)) {
    ++send_errors_;
    if ((send_errors_ & (send_errors_ - 1)) == 0)
      LOG(WARNING) << "crQuickShare: mDNS send failed; interface=" << index << "; system_error=" << network_->error();
    return false;
  }
  return true;
}
bool MdnsDiscovery::Announce(unsigned index, uint32_t ttl) {
  Packet packet; packet.flags = 0x8400; packet.answers = OwnRecords(index);
  for (auto& record : packet.answers) record.ttl = ttl;
  if (!Send(packet, index)) return false;
  ++announcements_; publications_[index].last_multicast = base::TimeTicks::Now(); return true;
}
void MdnsDiscovery::Publish(base::TimeTicks now) {
  for (auto& [index, publication] : publications_) {
    if (now < publication.due) continue;
    if (!publication.established && publication.probes < 3) {
      Packet probe;
      probe.questions = {{instance_, 255, 0x8001}, {host_, 255, 0x8001}};
      for (auto record : OwnRecords(index)) if (record.type != 12) {
        record.klass = 1; probe.authority.push_back(std::move(record));
      }
      if (Send(probe, index)) ++publication.probes;
      publication.due = now + base::Milliseconds(250); continue;
    }
    if (!publication.established) {
      publication.established = true;
      LOG(INFO) << "crQuickShare: mDNS probing complete; interface=" << index;
    }
    const bool announced = Announce(index, 120);
    if (announced) ++publication.announcements;
    publication.due = now + base::Seconds(!announced || publication.announcements == 1 ? 1 : 30);
  }
}
void MdnsDiscovery::Rename(bool instance_conflict, base::TimeTicks now) {
  for (const auto& [index, publication] : publications_) if (publication.established) Announce(index, 0);
  if (instance_conflict) {
    const auto previous = endpoint_;
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    do {
      endpoint_.clear();
      for (int n = 0; n < 4; ++n) endpoint_ += alphabet[base::RandGenerator(alphabet.size())];
    } while (endpoint_ == previous);
    SetNames();
  } else {
    host_ = "crquickshare-" + endpoint_ + "-" + base::HexEncode(base::RandBytesAsString(4)) + ".local";
  }
  if (conflict_window_.is_null() || now - conflict_window_ > base::Seconds(10)) { conflict_window_ = now; conflicts_ = 0; }
  ++conflicts_;
  ResetPublications(now, conflicts_ >= 15 ? base::Seconds(5) : base::TimeDelta());
  LOG(WARNING) << "crQuickShare: mDNS name conflict; endpoint_rotated=" << instance_conflict << "; probing_restarted=1";
}
bool MdnsDiscovery::Conflict(const Packet& packet, unsigned index, base::TimeTicks now) {
  auto found = publications_.find(index);
  if (found == publications_.end()) return false;
  auto& publication = found->second;
  const auto own = OwnRecords(index);
  if (packet.flags & 0x8000) {
    if (!publication.established && !publication.probes) return false;
    for (const auto* records : {&packet.answers, &packet.authority, &packet.additional}) for (const auto& record : *records) {
      if (!record.ttl || (record.klass & 0x7fff) != 1) continue;
      const bool instance_conflict = Canonical(record.name) == Canonical(instance_);
      if (!instance_conflict && Canonical(record.name) != Canonical(host_)) continue;
      // Unknown RR types do not contradict an established RRset; during ANY
      // probing they still indicate that this owner name is already in use.
      if (publication.established && std::ranges::none_of(own, [&](const auto& r) { return r.type == record.type && Canonical(r.name) == Canonical(record.name); })) continue;
      if (std::ranges::any_of(own, [&](const auto& r) { return mdns::Same(r, record); })) continue;
      if (publication.established) {
        // RFC 6762 section 9: re-probe, allowing the tie-break to decide ownership.
        for (const auto& [link, previous] : publications_) if (previous.established) Announce(link, 0);
        ResetPublications(now);
        LOG(WARNING) << "crQuickShare: mDNS established name conflict; probing_restarted=1";
      } else {
        Rename(instance_conflict, now);
      }
      return true;
    }
    return false;
  }
  if (publication.established || !publication.probes) return false;
  for (const auto& name : {instance_, host_}) {
    const bool asked = std::ranges::any_of(packet.questions, [&](const auto& q) {
      return Canonical(q.name) == Canonical(name) && (q.klass & 0x7fff) == 1;
    });
    if (!asked) continue;
    std::vector<std::string> theirs, ours;
    for (const auto& r : packet.authority) if ((r.klass & 0x7fff) == 1 && Canonical(r.name) == Canonical(name)) theirs.push_back(mdns::TieKey(r));
    if (theirs.empty()) continue;
    for (const auto& r : own) if (Canonical(r.name) == Canonical(name)) ours.push_back(mdns::TieKey(r));
    auto less = [](const auto& a, const auto& b) { return mdns::LessBytes(a, b); };
    std::ranges::sort(ours, less); std::ranges::sort(theirs, less);
    ours.erase(std::unique(ours.begin(), ours.end()), ours.end()); theirs.erase(std::unique(theirs.begin(), theirs.end()), theirs.end());
    if (std::lexicographical_compare(ours.begin(), ours.end(), theirs.begin(), theirs.end(), less)) {
      publication.probes = 0; publication.due = now + base::Seconds(1);
      LOG(INFO) << "crQuickShare: mDNS simultaneous probe deferred; interface=" << index;
      return true;
    }
  }
  return false;
}
void MdnsDiscovery::QueueReply(const Packet& packet, const MdnsDatagram& datagram, base::TimeTicks now) {
  const auto publication = publications_.find(datagram.interface_index);
  if (publication == publications_.end() || !publication->second.established) return;
  const auto own = OwnRecords(datagram.interface_index);
  const bool legacy = datagram.source.port != 5353;
  bool multicast = false, relevant = false;
  std::vector<Record> answers;
  for (const auto& question : packet.questions) for (const auto& record : own) if (Matches(question, record)) {
    relevant = true; multicast |= !(question.klass & 0x8000); AddUnique(answers, record);
  }
  auto pending = std::ranges::find_if(replies_, [&](const auto& reply) {
    return reply.index == datagram.interface_index && reply.source.address == datagram.source.address && reply.source.port == datagram.source.port &&
        reply.legacy == legacy && (!legacy || reply.id == packet.id);
  });
  if (!relevant && pending == replies_.end()) return;
  // A probe for an established unique name is answered immediately via QU;
  // its Authority section is not a known-answer list.
  const bool defending = !packet.authority.empty() && std::ranges::any_of(answers, [](const auto& r) { return r.type != 12; });
  if (pending == replies_.end()) {
    if (replies_.size() >= 64) return;
    PendingReply reply;
    reply.index = datagram.interface_index; reply.source = datagram.source;
    reply.unicast = legacy || !multicast; reply.legacy = legacy; reply.id = packet.id;
    reply.due = now + base::Milliseconds(defending || legacy ? 0 : base::RandIntInclusive(20, 120));
    reply.limit = now + base::Seconds(2);
    replies_.push_back(std::move(reply)); pending = std::prev(replies_.end());
  }
  if (relevant) {
    pending->unicast &= legacy || !multicast;
    if (legacy) pending->questions = packet.questions;
    for (const auto& answer : answers) AddUnique(pending->answers, answer);
    for (const auto& answer : answers) {
      if (answer.type == 12) for (const auto& record : own) if (record.type != 12) AddUnique(pending->additional, record);
      if (answer.type == 33) for (const auto& record : own) if (record.type == 1) AddUnique(pending->additional, record);
    }
  }
  if (!legacy) {
    auto suppress = [&](std::vector<Record>& records) {
      std::erase_if(records, [&](const auto& record) {
        const bool known = std::ranges::any_of(packet.answers, [&](const auto& answer) {
          return answer.ttl >= record.ttl / 2 && mdns::Same(answer, record);
        });
        suppressed_ += known; return known;
      });
    };
    suppress(pending->answers); suppress(pending->additional);
    if (packet.flags & 0x0200)
      pending->due = std::min(pending->limit, now + base::Milliseconds(base::RandIntInclusive(400, 500)));
    // RFC 6762 6: no more than one multicast answer per second. QU defense
    // stays immediate; packets from another querier cannot suppress this one.
    if (!pending->unicast) pending->due = std::max(pending->due, publication->second.last_multicast + base::Seconds(1));
  }
}
void MdnsDiscovery::FlushReplies(base::TimeTicks now) {
  for (auto it = replies_.begin(); it != replies_.end();) {
    if (!it->unicast && !it->answers.empty())
      it->due = std::max(it->due, publications_[it->index].last_multicast + base::Seconds(1));
    if (now < it->due) { ++it; continue; }
    if (!it->answers.empty()) {
      Packet response; response.flags = 0x8400; response.id = it->legacy ? it->id : 0;
      response.questions = std::move(it->questions); response.answers = std::move(it->answers);
      response.additional = std::move(it->additional);
      std::erase_if(response.additional, [&](const auto& record) {
        return std::ranges::any_of(response.answers, [&](const auto& answer) { return mdns::Same(record, answer); });
      });
      if (it->legacy) {
        for (auto& question : response.questions) question.klass &= 0x7fff;
        for (auto* records : {&response.answers, &response.additional}) for (auto& record : *records) { record.klass &= 0x7fff; record.ttl = std::min(record.ttl, 10u); }
      }
      if (Send(response, it->index, it->unicast ? &it->source : nullptr) && !it->unicast) {
        publications_[it->index].last_multicast = now;
        // The same multicast answer also satisfies other waiting QM queriers.
        for (auto& pending : replies_) if (!pending.unicast && pending.index == it->index) {
          auto answered = [&](const auto& record) {
            for (const auto* records : {&response.answers, &response.additional})
              if (std::ranges::any_of(*records, [&](const auto& sent) { return mdns::Same(sent, record); })) return true;
            return false;
          };
          std::erase_if(pending.answers, answered); std::erase_if(pending.additional, answered);
        }
      }
    }
    it = replies_.erase(it);
  }
}
void MdnsDiscovery::Cache(const std::vector<Record>& records, unsigned index, base::TimeTicks now) {
  std::map<CacheKey, std::vector<Record>> grouped;
  for (const auto& record : records) {
    if ((record.klass & 0x7fff) != 1) continue;
    const auto name = Canonical(record.name);
    const bool service = base::EndsWith(name, "." + Canonical(kService));
    if (record.type == 12) {
      if (name != Canonical(kService) || !base::EndsWith(Canonical(record.target), "." + Canonical(kService)) || Endpoint(record.target).empty()) continue;
    } else if (record.type == 33 || record.type == 16) {
      if (!service || record.data.size() > 1024) continue;
    } else if (record.type == 1) {
      if (!base::EndsWith(name, ".local") || record.data.size() != 4) continue;
      const uint32_t address = (uint32_t(Get16(record.data, 0)) << 16) | Get16(record.data, 2);
      if (!Local(address, index)) {
        if (AllowVisibilityLog(now))
          LOG(INFO) << "crQuickShare: mDNS address ignored; interface=" << index
                    << "; advertised_ipv4=" << AddressText(address) << "; reason=not_local_subnet";
        continue;
      }
    } else {
      continue;
    }
    auto& set = grouped[Key(index, record.name, record.type)];
    auto same = std::ranges::find_if(set, [&](const auto& r) { return mdns::Same(r, record); });
    if (same == set.end()) set.push_back(record);
    else { same->ttl = std::max(same->ttl, record.ttl); same->klass |= record.klass & 0x8000; }
  }
  const auto generation = ++generation_;
  for (const auto& [key, incoming] : grouped) {
    auto found = cache_.find(key);
    const bool positive = std::ranges::any_of(incoming, [](const auto& r) { return r.ttl != 0; });
    if (found == cache_.end()) {
      if (!positive || cache_.size() >= 512) continue;
      found = cache_.emplace(key, std::vector<Cached>()).first;
    }
    auto& set = found->second;
    const bool flush = std::get<2>(key) != 12 && std::ranges::any_of(incoming, [](const auto& r) { return r.ttl && (r.klass & 0x8000); });
    // Apply cache-flush to the entire incoming RRset, not one record at a time.
    // Preserve siblings in this packet and records younger than one second.
    if (flush) for (auto& existing : set) {
      if (now - existing.life.received < base::Seconds(1)) continue;
      if (std::ranges::none_of(incoming, [&](const auto& r) { return r.ttl && mdns::Same(r, existing.record); })) existing.life.Withdraw(now, "cache_flush");
    }
    for (const auto& record : incoming) {
      auto same = std::ranges::find_if(set, [&](const auto& existing) { return mdns::Same(existing.record, record); });
      if (!record.ttl) {
        if (same != set.end()) same->life.Withdraw(now, "goodbye");
        continue;
      }
      if (same == set.end()) {
        if (set.size() >= (record.type == 12 ? 128u : 16u)) continue;
        set.push_back({record, {}, generation}); same = std::prev(set.end());
      }
      same->record = record; same->generation = generation; same->life.Update(record.ttl, now);
    }
  }
}
const Cached* MdnsDiscovery::Select(unsigned index, std::string_view name, uint16_t type, base::TimeTicks now) const {
  const auto found = cache_.find(Key(index, name, type));
  if (found == cache_.end()) return nullptr;
  const Cached* selected = nullptr;
  for (const auto& record : found->second) {
    if (!record.life.Valid(now)) continue;
    // Prefer the current RRset over the one-second flush grace period. For
    // multiple values from one datagram use a stable order, not wire order.
    if (!selected || (selected->life.withdrawing && !record.life.withdrawing) ||
        (selected->life.withdrawing == record.life.withdrawing && (record.generation > selected->generation ||
         (record.generation == selected->generation && mdns::LessBytes(mdns::DataKey(record.record), mdns::DataKey(selected->record)))))) selected = &record;
  }
  return selected;
}
void MdnsDiscovery::Read(const MdnsDatagram& datagram, base::TimeTicks now) {
  if (!Local(datagram.source.address, datagram.interface_index)) {
    if (AllowVisibilityLog(now))
      LOG(INFO) << "crQuickShare: mDNS packet ignored; interface=" << datagram.interface_index
                << "; source_ipv4=" << AddressText(datagram.source.address)
                << "; source_port=" << datagram.source.port << "; reason=not_local_subnet";
    return;
  }
  const auto packet = mdns::Parse(datagram.bytes);
  if (!packet) { ++malformed_; return; }
  if ((packet->flags & 0x8000) && datagram.source.port != 5353) return;
  // A bridge may echo our advertisement through another local interface.
  // Match complete contents, not just source IP: another process on this host
  // can legitimately conflict with our instance or hostname.
  bool own_packet = std::ranges::any_of(interfaces_, [&](const auto& entry) { return entry.address == datagram.source.address; });
  if (own_packet) for (const auto* records : {&packet->answers, &packet->authority, &packet->additional}) for (const auto& record : *records) {
    bool ours = false;
    for (const auto& [index, publication] : publications_)
      ours |= std::ranges::any_of(OwnRecords(index), [&](const auto& r) { return mdns::Same(r, record); });
    own_packet &= ours;
  }
  if (!own_packet) Conflict(*packet, datagram.interface_index, now);
  if (!(packet->flags & 0x8000)) {
    QueueReply(*packet, datagram, now); return;  // Never cache other queriers' known answers.
  }
  // Responses marked truncated are not complete RRsets and cannot flush cache.
  if (packet->flags & 0x0200) return;
  std::vector<Record> records = packet->answers;
  records.insert(records.end(), packet->authority.begin(), packet->authority.end());
  records.insert(records.end(), packet->additional.begin(), packet->additional.end());
  Cache(records, datagram.interface_index, now);
}
void MdnsDiscovery::Query(std::string_view name, uint16_t type, unsigned index, base::TimeTicks now) {
  const auto key = Key(index, name, type);
  if (recent_queries_.contains(key) && recent_queries_.at(key) > now) return;
  if (recent_queries_.size() >= 4096) return;
  recent_queries_[key] = now + base::Seconds(1);
  Packet packet; packet.questions.push_back({std::string(name), type, 1});
  auto found = cache_.find(key);
  if (found != cache_.end()) for (const auto& cached : found->second) {
    if (!cached.life.Valid(now) || cached.life.withdrawing) continue;
    const auto remaining = (cached.life.until - now).InSeconds();
    if (remaining * 2 < cached.life.ttl) continue;
    auto answer = cached.record; answer.klass &= 0x7fff; answer.ttl = static_cast<uint32_t>(remaining);
    packet.answers.push_back(std::move(answer));
    if (mdns::Encode(packet).empty()) {
      auto overflow = std::move(packet.answers.back()); packet.answers.pop_back(); packet.flags |= 0x0200;
      if (Send(packet, index)) ++queries_;
      packet = {}; packet.answers.push_back(std::move(overflow));
    }
  }
  if (Send(packet, index)) ++queries_;
}
bool MdnsDiscovery::AllowVisibilityLog(base::TimeTicks now) {
  if (visibility_log_window_.is_null() || now - visibility_log_window_ >= base::Seconds(30)) {
    visibility_log_window_ = now; visibility_logs_ = 0;
  }
  if (visibility_logs_ >= 16) { ++visibility_logs_suppressed_; return false; }
  ++visibility_logs_; return true;
}
void MdnsDiscovery::DiagnoseVisibility(base::TimeTicks now) {
  // Inspect expired records before pruning, while their withdrawal causes are
  // still available. Report one first blocking dependency per transition.
  for (auto it = observed_services_.begin(); it != observed_services_.end();) {
    const auto& [index, service, unused_type] = it->first;
    const Cached* ptr = nullptr;
    const auto pointers = cache_.find(Key(index, kService, 12));
    if (pointers != cache_.end()) for (const auto& candidate : pointers->second)
      if (Canonical(candidate.record.target) == service) { ptr = &candidate; break; }
    const auto* srv = Select(index, service, 33, now);
    const auto* txt = Select(index, service, 16, now);
    const auto* address = srv ? Select(index, srv->record.target, 1, now) : nullptr;
    const auto info = txt ? EndpointInfo(txt->record) : std::string();
    const bool named = !EndpointName(info).empty();
    const bool ptr_live = ptr && ptr->life.Valid(now);
    if (ptr_live && srv && txt && Get16(srv->record.data, 4) && named && address) {
      it->second.target = srv->record.target; ++it; continue;
    }
    const bool target_changed = srv && Canonical(srv->record.target) != Canonical(it->second.target);
    const char* record_type = "PTR";
    const char* reason = "missing";
    const Cached* cause = ptr;
    auto expired = [&](std::string_view owner, uint16_t type) -> const Cached* {
      const auto found = cache_.find(Key(index, owner, type));
      if (found == cache_.end()) return nullptr;
      const Cached* latest = nullptr;
      for (const auto& candidate : found->second)
        if (!latest || candidate.life.until > latest->life.until) latest = &candidate;
      return latest;
    };
    if (!ptr_live) {
      // PTR is the root dependency even if other records expire together.
    } else if (!srv) { record_type = "SRV"; cause = expired(service, 33); }
    else if (!txt) { record_type = "TXT"; cause = expired(service, 16); }
    else if (!Get16(srv->record.data, 4)) { record_type = "SRV"; cause = srv; reason = "zero_port"; }
    else if (!named) {
      record_type = "TXT"; cause = txt;
      reason = !info.empty() && (uint8_t(info[0]) >> 5) > 1 ? "unsupported_info_version" :
          info.size() >= 17 && (uint8_t(info[0]) & 0x10) ? "name_not_public" : "invalid_endpoint_info";
    } else {
      record_type = "A"; cause = expired(srv->record.target, 1);
      if (target_changed) reason = "srv_target_unresolved";
    }
    if (cause && !cause->life.Valid(now)) reason = cause->life.removal;
    if (AllowVisibilityLog(now))
      LOG(INFO) << "crQuickShare: mDNS service unavailable; cache_id=" << it->second.id << "; interface=" << index
                << "; record=" << record_type << "; reason=" << reason << "; ptr_live=" << ptr_live
                << "; srv_live=" << bool(srv) << "; txt_live=" << bool(txt) << "; ipv4_live=" << bool(address)
                << "; info_bytes=" << info.size() << "; srv_target_changed=" << target_changed
                << "; record_ttl=" << (cause ? cause->life.ttl : 0);
    it = observed_services_.erase(it);
  }
}
void MdnsDiscovery::MaintainCache(base::TimeTicks now) {
  DiagnoseVisibility(now);
  std::erase_if(recent_queries_, [now](const auto& pair) { return pair.second <= now; });
  std::set<CacheKey> referenced_addresses;
  for (const auto& [key, set] : cache_) if (std::get<2>(key) == 33)
    for (const auto& record : set) if (record.life.Valid(now))
      referenced_addresses.insert(Key(std::get<0>(key), record.record.target, 1));
  for (auto it = cache_.begin(); it != cache_.end();) {
    std::erase_if(it->second, [now](const auto& record) { return !record.life.Valid(now); });
    if (it->second.empty()) { it = cache_.erase(it); continue; }
    const auto& [index, name, type] = it->first;
    bool refresh = false;
    if (type != 1 || referenced_addresses.contains(it->first))
      for (auto& record : it->second) refresh |= record.life.RefreshDue(now);
    if (refresh) Query(name, type, index, now);
    ++it;
  }
  std::set<CacheKey> active;
  size_t diagnostics = 0;
  for (const auto& [key, set] : cache_) {
    if (std::get<2>(key) != 12) continue;
    const auto index = std::get<0>(key);
    for (const auto& record : set) {
      if (Endpoint(record.record.target) == endpoint_) continue;
      const auto service_key = Key(index, record.record.target, 12);
      if (active.size() >= 128) break;
      active.insert(service_key);
      auto& resolution = resolutions_[service_key];
      if (!resolution.id) resolution.id = ++next_id_;
      const auto* srv = Select(index, record.record.target, 33, now);
      const auto* txt = Select(index, record.record.target, 16, now);
      const bool address = srv && Select(index, srv->record.target, 1, now);
      if (srv && txt && address && !Endpoint(record.record.target).empty() &&
          Get16(srv->record.data, 4) && !EndpointName(EndpointInfo(txt->record)).empty() &&
          !observed_services_.contains(service_key)) {
        observed_services_[service_key] = {resolution.id, srv->record.target};
        if (AllowVisibilityLog(now))
          LOG(INFO) << "crQuickShare: mDNS service available; cache_id=" << resolution.id << "; interface=" << index
                    << "; reason=records_ready";
      }
      if (now >= next_diagnostics_ && diagnostics++ < 16) {
        const auto info = txt ? EndpointInfo(txt->record) : std::string();
        const bool named = !EndpointName(info).empty();
        const char* reason = !srv ? "missing_srv" : !txt ? "missing_txt" :
            !Get16(srv->record.data, 4) ? "zero_port" : !named ? "name_unavailable" : !address ? "missing_ipv4" : "ready";
        std::string addresses;
        if (srv) {
          const auto found = cache_.find(Key(index, srv->record.target, 1));
          if (found != cache_.end()) for (const auto& entry : found->second) {
            if (!entry.life.Valid(now) || entry.record.data.size() != 4) continue;
            if (!addresses.empty()) addresses += ',';
            addresses += AddressText((uint32_t(Get16(entry.record.data, 0)) << 16) | Get16(entry.record.data, 2));
          }
        }
        LOG(INFO) << "crQuickShare: mDNS service diagnostics; cache_id=" << resolution.id << "; interface=" << index
                  << "; reason=" << reason << "; srv_live=" << bool(srv) << "; txt_live=" << bool(txt)
                  << "; ipv4_live=" << address << "; info_bytes=" << info.size()
                  << "; advertised_ipv4=" << (addresses.empty() ? "none" : addresses)
                  << "; service_port=" << (srv ? Get16(srv->record.data, 4) : 0)
                  << "; name_status=" << EndpointNameStatus(info)
                  << "; endpoint_id=" << base::HexEncode(Endpoint(record.record.target));
      }
      if (now < resolution.due) continue;
      if (srv && txt && address) { resolution.interval = 1; resolution.due = now + base::Seconds(1); continue; }
      resolution.due = now + base::Seconds(resolution.interval); resolution.interval = std::min(60, resolution.interval * 2);
      if (!srv) Query(record.record.target, 33, index, now);
      if (!txt) Query(record.record.target, 16, index, now);
      if (srv && !address) Query(srv->record.target, 1, index, now);
    }
  }
  std::erase_if(resolutions_, [&](const auto& pair) { return !active.contains(pair.first); });
}
std::vector<Peer> MdnsDiscovery::Peers() const {
  const auto now = base::TimeTicks::Now(); std::vector<Peer> peers;
  for (const auto& [key, set] : cache_) {
    if (std::get<2>(key) != 12) continue;
    const auto index = std::get<0>(key);
    for (const auto& ptr : set) {
      if (!ptr.life.Valid(now)) continue;
      const auto endpoint = Endpoint(ptr.record.target);
      if (endpoint.empty() || endpoint == endpoint_) continue;
      const auto* srv = Select(index, ptr.record.target, 33, now);
      const auto* txt = Select(index, ptr.record.target, 16, now);
      if (!srv || !txt) continue;
      const auto port = Get16(srv->record.data, 4);
      if (!port) continue;
      const auto info = EndpointInfo(txt->record);
      const char* info_error = nullptr;
      const auto decoded_info = ParseEndpointInfo(info, info_error);
      if (!decoded_info) continue;
      const auto name = decoded_info->name.value_or("");
      const auto addresses = cache_.find(Key(index, srv->record.target, 1));
      if (addresses == cache_.end()) continue;
      for (const auto& address : addresses->second) {
        if (!address.life.Valid(now)) continue;
        const auto& data = address.record.data;
        const auto ip = AddressText((uint32_t(Get16(data, 0)) << 16) | Get16(data, 2));
        const auto existing = std::ranges::find_if(peers, [&](const auto& peer) {
          return peer.id == endpoint && peer.endpoint_info == info && peer.port == port;
        });
        const size_t peer_index = existing - peers.begin();
        if (existing == peers.end()) {
          Peer peer; peer.id = endpoint; peer.name = name; peer.endpoint_info = info; peer.address = ip; peer.port = port;
          peer.discovery_interface = index;
          const auto resolution = resolutions_.find(Key(index, ptr.record.target, 12));
          if (resolution != resolutions_.end()) peer.discovery_cache_id = resolution->second.id;
          peers.push_back(std::move(peer));
        } else if (existing->address != ip && std::ranges::find(existing->alternate_addresses, ip) == existing->alternate_addresses.end()) {
          existing->alternate_addresses.push_back(ip);
        }
        auto& peer = peers[peer_index];
        const auto numeric_ip = ParseAddress(ip);
        for (const auto& link : interfaces_) {
          if (link.index != index || !numeric_ip || (*numeric_ip & link.mask) != (link.address & link.mask)) continue;
          LanRoute route{ip, AddressText(link.address), index};
          if (peer.routes.size() < 32 && std::ranges::find(peer.routes, route) == peer.routes.end())
            peer.routes.push_back(std::move(route));
        }
      }
    }
  }
  return peers;
}
void MdnsDiscovery::RestartQueries() { next_query_ = {}; query_interval_ = 1; next_network_check_ = {}; }
void MdnsDiscovery::RefreshAdvertisement() {
  for (auto& [index, publication] : publications_) if (publication.established) publication.due = {};
}
void MdnsDiscovery::Tick(bool visible) {
  const auto now = base::TimeTicks::Now(); RefreshNetwork(now);
  if (visible != visible_) {
    if (!visible) for (const auto& [index, publication] : publications_) if (publication.established) Announce(index, 0);
    visible_ = visible; ResetPublications(now); next_query_ = {}; query_interval_ = 1;
    LOG(INFO) << "crQuickShare: mDNS advertising requested; visible=" << visible_ << "; probing=" << visible_ << "; port=" << port_;
  }
  if (!ready()) return;
  for (int n = 0; n < 32; ++n) {
    MdnsDatagram datagram; const int result = network_->Receive(datagram);
    if (result < 0) { ++receive_errors_; break; }
    if (!result) break;
    ++received_; Read(datagram, now);
  }
  if (visible_) Publish(now);
  FlushReplies(now);
  MaintainCache(now);
  if (now >= next_query_) {
    std::set<unsigned> queried;
    for (const auto& entry : interfaces_) if (queried.insert(entry.index).second) Query(kService, 12, entry.index, now);
    next_query_ = now + base::Seconds(query_interval_); query_interval_ = std::min(60, query_interval_ * 2);
  }
  if (now >= next_diagnostics_) {
    next_diagnostics_ = now + base::Seconds(30);
    const auto candidates = Peers();
    const auto named = std::ranges::count_if(candidates, [](const Peer& peer) { return !peer.name.empty(); });
    size_t established = 0;
    for (const auto& [index, publication] : publications_) established += publication.established;
    LOG(INFO) << "crQuickShare: mDNS summary; counters=cumulative; queries_sent=" << queries_
              << "; packets_received=" << received_ << "; malformed_packets=" << malformed_
              << "; established_interfaces=" << established << "; announcement_packets=" << announcements_
              << "; known_answers_suppressed=" << suppressed_ << "; cached_rrsets=" << cache_.size()
              << "; visibility_logs_suppressed=" << visibility_logs_suppressed_
              << "; unchanged_network_notifications=" << unchanged_network_notifications_
              << "; lan_candidates=" << candidates.size() << "; visible_peers=" << named << "; send_errors=" << send_errors_ << "; receive_errors=" << receive_errors_;
  }
}
}  // namespace
std::unique_ptr<LanDiscovery> CreateLanDiscovery(const std::string& endpoint, const std::string& info, uint16_t port) {
  return std::make_unique<MdnsDiscovery>(endpoint, info, port);
}
}  // namespace crquickshare
