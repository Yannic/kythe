/*
 * Copyright 2018 The Kythe Authors. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "kythe/cxx/common/path_utils.h"

#include <stdlib.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/log/log.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "absl/synchronization/mutex.h"
#include "absl/types/span.h"
#include "kythe/cxx/common/regex.h"
#include "kythe/cxx/common/status.h"

namespace kythe {
namespace {

struct FreeDeleter {
  void operator()(void* pointer) const { free(pointer); }
};

// Predicate used in CleanPath for skipping empty components
// and components consistening of a single '.'.
struct SkipEmptyDot {
  bool operator()(absl::string_view sp) { return !(sp.empty() || sp == "."); }
};

// Deal with relative paths as well as '/' and '//'.
absl::string_view PathPrefix(absl::string_view path) {
  int slash_count = 0;
  for (char ch : path) {
    if (ch == '/' && ++slash_count <= 2) continue;
    break;
  }
  switch (slash_count) {
    case 0:
      return "";
    case 2:
      return "//";
    default:
      return "/";
  }
}

absl::string_view TrimPathPrefix(const absl::string_view path,
                                 absl::string_view prefix) {
  absl::string_view result = path;
  if (absl::ConsumePrefix(&result, prefix) &&
      (result.empty() || prefix == "/" || absl::ConsumePrefix(&result, "/"))) {
    return result;
  }
  return path;
}

absl::StatusOr<std::optional<PathRealizer>> MaybeMakeRealizer(
    PathCanonicalizer::Policy policy, absl::string_view root) {
  switch (policy) {
    case PathCanonicalizer::Policy::kCleanOnly:
      return {std::nullopt};
    case PathCanonicalizer::Policy::kPreferRelative:
    case PathCanonicalizer::Policy::kPreferReal:
      if (auto realizer = PathRealizer::Create(root); realizer.ok()) {
        return {*std::move(realizer)};
      } else {
        return realizer.status();
      }
  }
  return {std::nullopt};
}

std::optional<std::string> MaybeRealPath(
    const std::optional<PathRealizer>& realizer, absl::string_view root) {
  if (realizer.has_value()) {
    if (auto result = realizer->Relativize(root); result.ok()) {
      return *std::move(result);
    } else {
      LOG(ERROR) << "Unable to resolve " << root << ": " << result.status();
    }
  }
  return std::nullopt;
}

struct PathParts {
  absl::string_view dir, base;
};

PathParts SplitPath(absl::string_view path) {
  std::string::difference_type pos = path.find_last_of('/');

  // Handle the case with no '/' in 'path'.
  if (pos == absl::string_view::npos) return {path.substr(0, 0), path};

  // Handle the case with a single leading '/' in 'path'.
  if (pos == 0) return {path.substr(0, 1), absl::ClippedSubstr(path, 1)};

  return {path.substr(0, pos), absl::ClippedSubstr(path, pos + 1)};
}

constexpr struct VisitPattern {
  absl::string_view operator()(absl::string_view pattern) const {
    return pattern;
  }
  absl::string_view operator()(const Regex& pattern) const {
    return pattern.pattern();
  }
} kVisitPattern;

}  // namespace

absl::StatusOr<PathCleaner> PathCleaner::Create(absl::string_view root) {
  if (absl::StatusOr<std::string> resolved = MakeCleanAbsolutePath(root);
      resolved.ok()) {
    return PathCleaner(*std::move(resolved));
  } else {
    return resolved.status();
  }
}

absl::StatusOr<std::string> PathCleaner::Relativize(
    absl::string_view path) const {
  absl::StatusOr<std::string> clean_absolute_path = MakeCleanAbsolutePath(path);
  if (!clean_absolute_path.ok()) {
    // If we can't even make it a clean absolute path, return the error.
    return clean_absolute_path.status();
  }

  if (!kythe::IsAbsolutePath(path) &&
      !absl::StartsWith(*clean_absolute_path, root_)) {
    // If we were given a relative path, and its absolute version doesn't share
    // a prefix with the root, we can't relativize it simply by trimming off the
    // prefix. Return the original path.
    return std::string(path);
  }

  // Proceed with original logic, using the already computed clean_absolute_path
  return std::string(TrimPathPrefix(*clean_absolute_path, root_));
}

absl::StatusOr<PathRealizer> PathRealizer::Create(absl::string_view root) {
  if (absl::StatusOr<std::string> resolved = RealPath(root); resolved.ok()) {
    return PathRealizer(*std::move(resolved));
  } else {
    return resolved.status();
  }
}

// We do not copy the cache on assignment or construction to retain thread
// safety.
PathRealizer::PathRealizer(const PathRealizer& other) : root_(other.root_) {}
PathRealizer& PathRealizer::operator=(const PathRealizer& other) {
  root_ = other.root_;
  return *this;
}

template <typename K, typename Fn>
absl::StatusOr<std::string> PathRealizer::PathCache::FindOrInsert(K&& key,
                                                                  Fn&& make) {
  absl::MutexLock lock(&mu_);
  auto [iter, inserted] = cache_.try_emplace(std::forward<K>(key), "");
  if (inserted) {
    iter->second = std::forward<Fn>(make)();
  }
  return iter->second;
}

absl::StatusOr<std::string> PathRealizer::Relativize(
    absl::string_view path) const {
  return cache_->FindOrInsert(
      CleanPath(path), [this, path]() -> absl::StatusOr<std::string> {
        if (absl::StatusOr<std::string> resolved = RealPath(path);
            resolved.ok()) {
          return std::string(TrimPathPrefix(*std::move(resolved), root_));
        } else {
          return resolved.status();
        }
      });
}

absl::StatusOr<PathCanonicalizer> PathCanonicalizer::Create(
    absl::string_view root, Policy policy,
    absl::Span<const PathEntry> path_map) {
  absl::StatusOr<PathCleaner> cleaner = PathCleaner::Create(root);
  if (!cleaner.ok()) {
    return cleaner.status();
  }
  absl::StatusOr<std::optional<PathRealizer>> realizer =
      MaybeMakeRealizer(policy, root);
  if (!realizer.ok()) {
    return realizer.status();
  }

  std::vector<Policy> override_policies;
  std::vector<absl::string_view> override_paths;
  for (const auto& [path, policy] : path_map) {
    if (!realizer->has_value()) {
      realizer = MaybeMakeRealizer(policy, root);
      if (!realizer.ok()) {
        return realizer.status();
      }
    }
    override_policies.push_back(policy);
    override_paths.push_back(std::visit(kVisitPattern, path));
  }
  absl::StatusOr<RegexSet> override_set = RegexSet::Build(override_paths);
  if (!override_set.ok()) {
    return override_set.status();
  }
  return PathCanonicalizer(policy, *std::move(cleaner), *std::move(realizer),
                           *std::move(override_set),
                           std::move(override_policies));
}

absl::StatusOr<std::string> PathCanonicalizer::Relativize(
    absl::string_view path) const {
  absl::StatusOr<Policy> policy = PolicyFor(path);
  if (!policy.ok()) return policy.status();

  switch (*policy) {
    case Policy::kPreferRelative:
      if (auto resolved = MaybeRealPath(realizer_, path)) {
        if (!IsAbsolutePath(*resolved)) {
          return *std::move(resolved);
        }
      }
      return cleaner_.Relativize(path);
    case Policy::kPreferReal:
      if (auto resolved = MaybeRealPath(realizer_, path)) {
        return *std::move(resolved);
      }
      return cleaner_.Relativize(path);
    case Policy::kCleanOnly:
      return cleaner_.Relativize(path);
  }
  LOG(FATAL) << "Unknown policy: " << static_cast<int>(*policy);
  return std::string(path);
}

absl::StatusOr<PathCanonicalizer::Policy> PathCanonicalizer::PolicyFor(
    absl::string_view path) const {
  absl::StatusOr<std::vector<int>> match = override_set_.ExplainMatch(path);
  if (!match.ok()) {
    return match.status();
  }
  if (match->empty()) {
    return policy_;
  }
  return override_policy_[*absl::c_min_element(*match)];
}

std::optional<PathCanonicalizer::Policy> ParseCanonicalizationPolicy(
    absl::string_view policy) {
  using Policy = PathCanonicalizer::Policy;
  if (policy == "0" || policy == "clean-only") {
    return Policy::kCleanOnly;
  }
  if (policy == "1" || policy == "prefer-relative") {
    return Policy::kPreferRelative;
  }
  if (policy == "2" || policy == "prefer-real") {
    return Policy::kPreferReal;
  }
  return std::nullopt;
}

bool AbslParseFlag(absl::string_view text, PathCanonicalizer::Policy* policy,
                   std::string* error) {
  if (auto parsed = ParseCanonicalizationPolicy(text)) {
    *policy = *parsed;
    return true;
  }
  *error = "policy not one of: clean-only, prefer-relative, prefer-real";
  return false;
}

std::string AbslUnparseFlag(PathCanonicalizer::Policy policy) {
  using Policy = PathCanonicalizer::Policy;
  switch (policy) {
    case Policy::kCleanOnly:
      return "clean-only";
    case Policy::kPreferRelative:
      return "prefer-relative";
    case Policy::kPreferReal:
      return "prefer-real";
  }
  LOG(FATAL) << "Invalid path policy provided: " << static_cast<int>(policy);
  return "(unknown)";
}

std::string JoinPath(absl::string_view a, absl::string_view b) {
  return absl::StrCat(absl::StripSuffix(a, "/"), "/",
                      absl::StripPrefix(b, "/"));
}

bool AbslParseFlag(absl::string_view text, PathCanonicalizer::PathEntry* entry,
                   std::string* error) {
  size_t pos = text.find('@');
  if (pos == text.npos) {
    *error = "missing @ delimiter between path and policy";
    return false;
  }
  absl::StatusOr<Regex> path = Regex::Compile(text.substr(0, pos));
  if (!path.ok()) {
    *error = path.status().message();
    return false;
  }
  entry->path = *std::move(path);
  return AbslParseFlag(text.substr(pos + 1), &entry->policy, error);
}

std::string AbslUnparseFlag(const PathCanonicalizer::PathEntry& entry) {
  return absl::StrCat(std::visit(kVisitPattern, entry.path), "@",
                      AbslUnparseFlag(entry.policy));
}

bool AbslParseFlag(absl::string_view text,
                   std::vector<PathCanonicalizer::PathEntry>* entries,
                   std::string* error) {
  for (const auto& entry : absl::StrSplit(text, ' ', absl::SkipEmpty())) {
    if (!AbslParseFlag(entry, &entries->emplace_back(), error)) {
      entries->pop_back();
      return false;
    }
  }
  return true;
}

std::string AbslUnparseFlag(
    const std::vector<PathCanonicalizer::PathEntry>& entries) {
  return absl::StrJoin(entries, " ", [](std::string* out, const auto& entry) {
    absl::StrAppend(out, AbslUnparseFlag(entry));
  });
}

std::string CleanPath(absl::string_view input) {
  const bool is_absolute_path = absl::StartsWith(input, "/");
  std::vector<absl::string_view> parts;
  for (absl::string_view comp : absl::StrSplit(input, '/', SkipEmptyDot{})) {
    if (comp == "..") {
      if (!parts.empty() && parts.back() != "..") {
        parts.pop_back();
        continue;
      }
      if (is_absolute_path) continue;
    }
    parts.push_back(comp);
  }
  // Deal with leading '//' as well as '/'.
  return absl::StrCat(PathPrefix(input), absl::StrJoin(parts, "/"));
}

bool IsAbsolutePath(absl::string_view path) {
  return absl::StartsWith(path, "/");
}

absl::StatusOr<std::string> GetCurrentDirectory() {
  std::string result(128, '\0');
  while (::getcwd(&result.front(), result.size() + 1) == nullptr) {
    if (errno != ERANGE) {
      return ErrnoToStatus(errno);
    }
    result.resize(result.size() * 2);
  }
  result.resize(::strlen(result.data()));
  return result;
}

absl::StatusOr<std::string> MakeCleanAbsolutePath(absl::string_view path) {
  if (IsAbsolutePath(path)) {
    return CleanPath(path);
  }
  if (absl::StatusOr<std::string> dir = GetCurrentDirectory(); dir.ok()) {
    return CleanPath(JoinPath(*std::move(dir), path));
  } else {
    return dir.status();
  }
}

absl::string_view Dirname(absl::string_view path) {
  return SplitPath(path).dir;
}

absl::string_view Basename(absl::string_view path) {
  return SplitPath(path).base;
}

std::string RelativizePath(absl::string_view to_relativize,
                           absl::string_view relativize_against) {
  absl::StatusOr<PathCleaner> cleaner = PathCleaner::Create(relativize_against);
  if (!cleaner.ok()) {
    return "";
  }
  return cleaner->Relativize(to_relativize).value_or("");
}

absl::StatusOr<std::string> RealPath(absl::string_view path) {
  // realpath requires a null-terminated cstring, but string_view may not be.
  // checking whether or not it is null-terminated is potentially UB.
  std::string zpath(path);

  std::unique_ptr<char, FreeDeleter> resolved(
      ::realpath(zpath.c_str(), nullptr));
  if (resolved == nullptr) {
    return ErrnoToStatus(errno);
  }
  return std::string(resolved.get());
}

}  // namespace kythe
