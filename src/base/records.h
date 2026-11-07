#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace mico {

struct RecordSpan {
  size_t from = 0, size = 0;
};

struct RecordRecovery {
  bool damaged = false;
  std::vector<RecordSpan> records;
};

// A failed append can leave a cut-off record joined to the next one. Look
// for another copy of the opening record header, validate candidates, and
// retain complete objects in file order. Healthy lines take a substring
// search, without parsing their payloads or allocating. Recovery is bounded
// and never rewrites the file; an unfinished final physical line is the
// caller's responsibility to hold back until its newline arrives.
RecordRecovery recover_records(std::string_view line);

template <class F>
bool for_each_record(std::string_view line, F&& fn) {
  const auto recovery = recover_records(line);
  if (!recovery.damaged) return fn(line, size_t(0));
  for (const auto& record : recovery.records)
    if (!fn(line.substr(record.from, record.size), record.from)) return false;
  return true;
}

}  // namespace mico
