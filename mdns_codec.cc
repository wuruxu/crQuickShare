// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/mdns_codec.h"
#include <algorithm>
#include <utility>
#include "base/strings/string_util.h"
namespace crquickshare::mdns {
std::string Canonical(std::string_view name) { return base::ToLowerASCII(name); }
void U16(std::string& out, uint16_t value) { out += char(value >> 8); out += char(value); }
void U32(std::string& out, uint32_t value) { U16(out, value >> 16); U16(out, value); }
uint16_t Get16(std::string_view bytes, size_t offset) {
  return (uint16_t(uint8_t(bytes[offset])) << 8) | uint8_t(bytes[offset + 1]);
}
std::string Name(std::string_view name) {
  std::string out, label;
  for (size_t at = 0; at < name.size(); ++at) {
    const char c = name[at];
    if (c == '.') {
      if (label.empty() || label.size() > 63) return {};
      out += char(label.size()); out += label; label.clear();
    } else if (c == '\\') {
      // Presentation names escape arbitrary label bytes as three decimals.
      if (at + 3 >= name.size()) return {};
      unsigned value = 0;
      for (size_t digit = 1; digit <= 3; ++digit) {
        if (name[at + digit] < '0' || name[at + digit] > '9') return {};
        value = value * 10 + unsigned(name[at + digit] - '0');
      }
      if (value > 255) return {};
      label += char(value); at += 3;
    } else {
      label += c;
    }
    if (label.size() > 63 || out.size() + label.size() > 253) return {};
  }
  if (!label.empty()) { out += char(label.size()); out += label; }
  else if (!name.empty()) return {};
  out += '\0'; return out;
}
namespace {
class Reader {
 public:
  explicit Reader(std::string_view bytes) : bytes_(bytes) {}
  size_t ReadName(size_t offset, std::string& name) const {
    name.clear(); size_t consumed = 0, expanded = 1; bool jumped = false;
    for (unsigned hops = 0; hops < 128; ++hops) {
      if (offset >= bytes_.size()) return 0;
      const auto length = uint8_t(bytes_[offset]);
      if ((length & 0xc0) == 0xc0) {
        if (offset + 1 >= bytes_.size()) return 0;
        const size_t target = ((length & 0x3f) << 8) | uint8_t(bytes_[offset + 1]);
        if (target < 12 || target >= offset) return 0;
        if (!jumped) consumed += 2;
        jumped = true; offset = target; continue;
      }
      if (length > 63 || offset + 1 + length > bytes_.size()) return 0;
      if (!jumped) consumed += length + 1;
      ++offset;
      if (!length) return consumed;
      expanded += length + 1;
      if (expanded > 255) return 0;
      if (!name.empty()) name += '.';
      for (size_t n = 0; n < length; ++n) {
        const auto c = uint8_t(bytes_[offset + n]);
        if (c < 32 || c > 126 || c == '.' || c == '\\') {
          name += '\\'; name += char('0' + c / 100); name += char('0' + (c / 10) % 10); name += char('0' + c % 10);
        } else {
          name += char(c);
        }
      }
      offset += length;
    }
    return 0;
  }
  bool ReadRecords(size_t count, size_t& offset, std::vector<Record>& records) const {
    for (size_t n = 0; n < count; ++n) {
      Record record;
      const auto consumed = ReadName(offset, record.name);
      if (!consumed || offset + consumed + 10 > bytes_.size()) return false;
      offset += consumed;
      record.type = Get16(bytes_, offset); record.klass = Get16(bytes_, offset + 2);
      record.ttl = (uint32_t(Get16(bytes_, offset + 4)) << 16) | Get16(bytes_, offset + 6);
      const size_t length = Get16(bytes_, offset + 8); offset += 10;
      if (length > bytes_.size() - offset) return false;
      record.data = bytes_.substr(offset, length);
      if (record.type == 12 || record.type == 33) {
        const size_t prefix = record.type == 33 ? 6 : 0;
        if (length <= prefix || ReadName(offset + prefix, record.target) != length - prefix) return false;
        record.data = record.data.substr(0, prefix) + Name(record.target);
      } else if (record.type == 1 && length != 4) {
        return false;
      } else if (record.type == 16) {
        for (size_t at = 0; at < length;) {
          const size_t size = uint8_t(record.data[at++]);
          if (size > length - at) return false;
          at += size;
        }
      }
      offset += length; records.push_back(std::move(record));
    }
    return true;
  }
 private:
  std::string_view bytes_;
};
}
std::optional<Packet> Parse(std::string_view bytes) {
  if (bytes.size() < 12 || bytes.size() > kMaxPacket || (Get16(bytes, 2) & 0x780f)) return std::nullopt;
  const size_t questions = Get16(bytes, 4), answers = Get16(bytes, 6), authority = Get16(bytes, 8), additional = Get16(bytes, 10);
  if (questions > 64 || answers + authority + additional > 128) return std::nullopt;
  Packet result; result.id = Get16(bytes, 0); result.flags = Get16(bytes, 2);
  Reader reader(bytes); size_t offset = 12;
  for (size_t n = 0; n < questions; ++n) {
    Question question; const auto consumed = reader.ReadName(offset, question.name);
    if (!consumed || offset + consumed + 4 > bytes.size()) return std::nullopt;
    offset += consumed;
    question.type = Get16(bytes, offset); question.klass = Get16(bytes, offset + 2); offset += 4;
    result.questions.push_back(std::move(question));
  }
  if (!reader.ReadRecords(answers, offset, result.answers) || !reader.ReadRecords(authority, offset, result.authority) ||
      !reader.ReadRecords(additional, offset, result.additional) || offset != bytes.size()) return std::nullopt;
  return result;
}
std::string Encode(const Packet& packet) {
  if (packet.questions.size() > 64 || packet.answers.size() + packet.authority.size() + packet.additional.size() > 128) return {};
  std::string out; U16(out, packet.id); U16(out, packet.flags); U16(out, packet.questions.size());
  U16(out, packet.answers.size()); U16(out, packet.authority.size()); U16(out, packet.additional.size());
  for (const auto& question : packet.questions) {
    const auto name = Name(question.name); if (name.empty()) return {};
    out += name; U16(out, question.type); U16(out, question.klass);
  }
  for (const auto* records : {&packet.answers, &packet.authority, &packet.additional}) for (const auto& record : *records) {
    const auto name = Name(record.name);
    if (name.empty() || record.data.size() > 65535) return {};
    out += name; U16(out, record.type); U16(out, record.klass); U32(out, record.ttl);
    U16(out, record.data.size()); out += record.data;
    if (out.size() > kMaxPacket) return {};
  }
  return out.size() <= kMaxPacket ? out : std::string();
}
std::string DataKey(const Record& record) {
  if (record.type == 12) return Name(Canonical(record.target));
  if (record.type == 33) return record.data.substr(0, 6) + Name(Canonical(record.target));
  return record.data;
}
bool Same(const Record& first, const Record& second) {
  return Canonical(first.name) == Canonical(second.name) && first.type == second.type &&
      (first.klass & 0x7fff) == (second.klass & 0x7fff) && DataKey(first) == DataKey(second);
}
std::string TieKey(const Record& record) {
  std::string out; U16(out, record.klass & 0x7fff); U16(out, record.type); out += DataKey(record); return out;
}
bool LessBytes(std::string_view first, std::string_view second) {
  return std::lexicographical_compare(first.begin(), first.end(), second.begin(), second.end(),
      [](char a, char b) { return uint8_t(a) < uint8_t(b); });
}
}  // namespace crquickshare::mdns
