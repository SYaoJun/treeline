#include <deque>
#include <utility>
#include <vector>

#include "../bufmgr/page_memory_allocator.h"
#include "circular_page_buffer.h"
#include "manager.h"
#include "persist/merge_iterator.h"
#include "persist/page.h"
#include "persist/segment_wrap.h"
#include "util/key.h"

namespace {

using namespace llsm;
using namespace llsm::pg;

class PageChain {
 public:
  PageChain() : main_page_(nullptr), overflow_page_(nullptr) {}

  static PageChain SingleOnly(void* main) { return PageChain(main, nullptr); }
  static PageChain WithOverflow(void* main, void* overflow) {
    return PageChain(main, overflow);
  }

  pg::Page main() const { return pg::Page(main_page_); }
  std::optional<pg::Page> overflow() const {
    return overflow_page_ == nullptr ? std::optional<pg::Page>()
                                     : pg::Page(overflow_page_);
  }

  PageMergeIterator GetIterator() const {
    if (overflow_page_ != nullptr) {
      return PageMergeIterator(
          {main().GetIterator(), overflow()->GetIterator()});
    } else {
      return PageMergeIterator({main().GetIterator()});
    }
  }

  size_t NumPages() const { return overflow_page_ == nullptr ? 1 : 2; }

  // Returns the largest key in the chain. If the chain is empty, there is no
  // largest key.
  std::optional<Key> LargestKey() const {
    std::optional<Key> largest;
    auto main_it = main().GetIterator();
    main_it.SeekToLast();
    if (!main_it.Valid() && overflow_page_ == nullptr) {
      // The chain is empty. There is no largest key.
      return largest;
    }
    if (main_it.Valid()) {
      largest = key_utils::ExtractHead64(main_it.key());
    }
    if (overflow_page_ != nullptr) {
      auto overflow_it = overflow()->GetIterator();
      overflow_it.SeekToLast();
      if (overflow_it.Valid()) {
        const Key overflow_largest =
            key_utils::ExtractHead64(overflow_it.key());
        if (!largest.has_value() || overflow_largest > *largest) {
          largest = overflow_largest;
        }
      }
    }
    return largest;
  }

 private:
  PageChain(void* main, void* overflow)
      : main_page_(main), overflow_page_(overflow) {}

  void* main_page_;
  void* overflow_page_;
};

class PagePlusRecordMerger {
 public:
  PagePlusRecordMerger(std::vector<Record>::const_iterator rec_begin,
                       std::vector<Record>::const_iterator rec_end)
      : it_(rec_begin), it_end_(rec_end) {}

  bool HasPageRecords() const { return pmi_.Valid(); }
  bool HasRecords() const { return HasPageRecords() || it_ != it_end_; }

  Record GetNext() {
    assert(HasRecords());
    // Simple case - one of the iterators is empty.
    if (!HasPageRecords()) {
      assert(it_ != it_end_);
      const Record r = *it_;
      ++it_;
      return r;
    } else if (it_ == it_end_) {
      const Key k = key_utils::ExtractHead64(pmi_.key());
      const Record r(k, pmi_.value());
      pmi_.Next();
      return r;
    }

    // Merge case. Select the smaller key. Prefer the in memory record if the
    // keys are equal.
    const Key pmi_key = key_utils::ExtractHead64(pmi_.key());
    const Key it_key = it_->first;
    if (it_key <= pmi_key) {
      const Record r = *it_;
      ++it_;
      if (it_key == pmi_key) {
        pmi_.Next();
      }
      return r;
    } else {
      // it_key > pmi_key
      const Record r(pmi_key, pmi_.value());
      pmi_.Next();
      return r;
    }
  }

  void UpdatePageIterator(PageMergeIterator pmi) { pmi_ = pmi; }

 private:
  PageMergeIterator pmi_;
  std::vector<Record>::const_iterator it_;
  std::vector<Record>::const_iterator it_end_;
};

}  // namespace

namespace llsm {
namespace pg {

void Manager::RewriteSegments(
    Key segment_base, std::vector<Record>::const_iterator addtl_rec_begin,
    std::vector<Record>::const_iterator addtl_rec_end) {
  // TODO: Multi-threading concerns.
  std::vector<std::pair<Key, SegmentInfo*>> segments_to_rewrite;
  std::vector<std::pair<Key, SegmentInfo>> rewritten_segments;
  std::vector<SegmentId> overflows_to_clear;

  const auto it = index_.lower_bound(segment_base);
  assert(it != index_.end());
  segments_to_rewrite.emplace_back(segment_base, &(it->second));

  // 1. Look up neighboring segments that can benefit from a rewrite.
  if (options_.consider_neighbors_during_rewrite) {
    // Scan backward.
    if (it != index_.begin()) {
      auto prev_it(it);
      while (true) {
        --prev_it;
        if (!prev_it->second.HasOverflow()) break;
        segments_to_rewrite.emplace_back(prev_it->first, &(prev_it->second));
        if (prev_it == index_.begin()) break;
      }
    }

    // Scan forward.
    auto next_it(it);
    ++next_it;
    for (; next_it != index_.end(); ++next_it) {
      if (!next_it->second.HasOverflow()) break;
      segments_to_rewrite.emplace_back(next_it->first, &(next_it->second));
    }

    // Sort the segments.
    std::sort(segments_to_rewrite.begin(), segments_to_rewrite.end(),
              [](const std::pair<Key, SegmentInfo*>& seg1,
                 const std::pair<Key, SegmentInfo*>& seg2) {
                return seg1.first < seg2.first;
              });
  }

  // 2. Load and merge the segments.
  //
  // General approach:
  // We use a logical "sliding window" over the segments to rewrite. We read in
  // segments, feed their records through a `SegmentBuilder`, and write out the
  // results into new segments on disk. We do this to (i) avoid reading all the
  // pages into memory at once and (ii) to do the rewrite in one pass.
  //
  // How large is the sliding window? We use a window of 4 * 16 = 64 pages.
  //
  // The largest segment we allow is 16 pages. Each segment can have up to one
  // overflow per page, meaning there are at most 16 overflows per segment.
  // Thus, assuming that the segments were built using the same goal and delta
  // parameters as the rewrite, we will span at most two 16 page segments in the
  // worst case.
  //
  // If this assumption does not hold, we may need to read in more data in the
  // worst case to fully "pack" a 16 page segment. If this happens and there is
  // no more memory available in our sliding window, we will just write
  // currently-being-built built segment onto disk instead.

  // Used for recovery.
  const uint32_t sequence_number = next_sequence_number_++;

  CircularPageBuffer page_buf(SegmentBuilder::kSegmentPageCounts.back() * 4);
  SegmentBuilder seg_builder(options_.records_per_page_goal,
                             options_.records_per_page_delta);

  // Keeps track of the pages in memory (the "sliding window"). The pages'
  // backing memory is in `page_buf`. The page chains in the deques are sorted
  // in ascending order (by key).
  //
  // `pages_to_process` contains page chains that need to be offered to the
  // `SegmentBuilder`. `pages_processed` contains chains that have been offered
  // to the `SegmentBuilder` but who still need to be kept around in memory
  // (because their records have not yet been written to new segments).
  std::deque<PageChain> pages_to_process, pages_processed;

  PagePlusRecordMerger pm(addtl_rec_begin, addtl_rec_end);

  // TODO: Before starting the rewrite, we should log (and force to stable
  // storage) the following:
  // - The rewrite sequence number: `sequence_number`
  // - A list of all the segments involved in the rewrite

  const auto load_into_segments_and_free_pages =
      [this, &pages_processed, &rewritten_segments, &seg_builder, &page_buf,
       sequence_number](const std::vector<Segment>& segments) {
        // Load records into the new segments and write them to disk.
        for (size_t i = 0; i < segments.size(); ++i) {
          const Segment& seg = segments[i];
          // The upper bound is used for the page format (for computing shared
          // key prefixes).
          Key upper_bound;
          if (i < segments.size() - 1) {
            upper_bound = segments[i + 1].base_key;
          } else {
            const auto maybe_key = seg_builder.CurrentBaseKey();
            if (maybe_key.has_value()) {
              upper_bound = *maybe_key;
            } else {
              // The segment builder is "empty". This happens in two situations:
              // 1. We need to read in the next segment but do not have enough
              //    memory to hold it, so we are flushing all the currently
              //    processed records into segments.
              // 2. We are writing out the last group of records in this
              //    rewrite.
              // To find the upper bound, we query the index to find the left
              // boundary of the next segment (if it exists).
              const auto it =
                  index_.upper_bound(segments.back().records.back().first);
              if (it != index_.end()) {
                upper_bound = it->first;
              } else {
                // We are rewriting the last segment in the database.
                upper_bound = std::numeric_limits<Key>::max();
              }
            }
          }
          rewritten_segments.emplace_back(
              LoadIntoNewSegment(sequence_number, seg, upper_bound));
        }

        // "Remove" no longer needed pages from memory.
        // All processed page chains storing keys before this key are no longer
        // needed.
        const auto maybe_key = seg_builder.CurrentBaseKey();
        if (maybe_key.has_value()) {
          const Key next_key = *maybe_key;
          // Check page chains one by one.
          while (!pages_processed.empty()) {
            // Check if it is safe to remove the page chain from memory.
            const auto largest_key = pages_processed.front().LargestKey();
            if (largest_key.has_value() && largest_key >= next_key) {
              // This page chain and the ones following it contain keys that
              // have not been written out.
              break;
            }
            // Safe to deallocate this page chain.
            const size_t num_pages = pages_processed.front().NumPages();
            for (size_t i = 0; i < num_pages; ++i) {
              page_buf.Free();
            }
            pages_processed.pop_front();
          }

        } else {
          // All processed page chains can be removed (the segment builder is
          // empty).
          while (!pages_processed.empty()) {
            const size_t num_pages = pages_processed.front().NumPages();
            for (size_t i = 0; i < num_pages; ++i) {
              page_buf.Free();
            }
            pages_processed.pop_front();
          }
        }
      };

  for (const auto& seg_to_rewrite : segments_to_rewrite) {
    const size_t segment_pages = seg_to_rewrite.second->page_count();
    if (segment_pages > page_buf.NumFreePages()) {
      // Not enough memory to read the next segment. Flush the
      // currently-being-built segment to disk.
      const auto segments = seg_builder.Finish();
      assert(segments.size() > 0);
      load_into_segments_and_free_pages(segments);
      assert(segment_pages <= page_buf.NumFreePages());
    }

    // Load the segment and check for overflows.
    ReadSegment(seg_to_rewrite.second->id());
    SegmentWrap sw(w_.buffer().get(), seg_to_rewrite.second->page_count());
    const size_t num_overflows = sw.NumOverflows();
    if (segment_pages + num_overflows > page_buf.NumFreePages()) {
      // Not enough memory to read the next segment's overflows. Flush the
      // currently-being-built segment to disk.
      const auto segments = seg_builder.Finish();
      assert(segments.size() > 0);
      load_into_segments_and_free_pages(segments);
      assert(segment_pages + num_overflows <= page_buf.NumFreePages());
    }

    // Copy the segment pages into the buffer, leaving room for overflows as
    // needed.
    std::vector<PageChain> chains_in_segment;
    std::vector<std::pair<SegmentId, void*>> overflows_to_load;
    sw.ForEachPage([&](pg::Page page) {
      if (page.HasOverflow()) {
        void* main_page = page_buf.Allocate();
        void* overflow_page = page_buf.Allocate();
        memcpy(main_page, page.data().data(), page.data().size());
        chains_in_segment.push_back(
            PageChain::WithOverflow(main_page, overflow_page));
        overflows_to_load.emplace_back(page.GetOverflow(), overflow_page);
        overflows_to_clear.emplace_back(page.GetOverflow());

      } else {
        void* main_page = page_buf.Allocate();
        memcpy(main_page, page.data().data(), page.data().size());
        chains_in_segment.push_back(PageChain::SingleOnly(main_page));
      }
    });

    // Load all overflows into memory.
    ReadOverflows(overflows_to_load);

    // Add chains into the deque.
    pages_to_process.insert(pages_to_process.end(), chains_in_segment.begin(),
                            chains_in_segment.end());

    // Process the pages in the deque.
    while (!pages_to_process.empty()) {
      const PageChain& pc = pages_to_process.front();
      pm.UpdatePageIterator(pc.GetIterator());

      while (pm.HasPageRecords()) {
        auto segments = seg_builder.Offer(pm.GetNext());
        if (segments.size() == 0) continue;
        load_into_segments_and_free_pages(segments);
      }

      // Done with this page chain.
      pages_processed.push_back(pages_to_process.front());
      pages_to_process.pop_front();
    }
  }

  assert(!pm.HasPageRecords());

  // Handle any leftover in-memory records.
  while (pm.HasRecords()) {
    auto segments = seg_builder.Offer(pm.GetNext());
    if (segments.size() == 0) continue;
    load_into_segments_and_free_pages(segments);
  }

  // Handle any leftover segments.
  auto segments = seg_builder.Finish();
  if (segments.size() > 0) {
    load_into_segments_and_free_pages(segments);
  }

  // 3. Update the free list and mark the old segments as invalid. To do this we
  // zero out the first page in each segment.
  void* zero = w_.buffer().get();
  memset(zero, 0, pg::Page::kSize);
  std::vector<std::future<void>> write_futures;
  if (bg_threads_ != nullptr) {
    write_futures.reserve(segments_to_rewrite.size() +
                          overflows_to_clear.size());
    for (const auto& seg_to_rewrite : segments_to_rewrite) {
      const SegmentId seg_id = seg_to_rewrite.second->id();
      write_futures.push_back(bg_threads_->Submit(
          [this, seg_id, zero]() { WritePage(seg_id, 0, zero); }));
      free_.Add(seg_id);
    }
    for (const auto& overflow_to_clear : overflows_to_clear) {
      write_futures.push_back(
          bg_threads_->Submit([this, overflow_to_clear, zero]() {
            WritePage(overflow_to_clear, 0, zero);
          }));
      free_.Add(overflow_to_clear);
    }
    // Important that the page at `zero` is not modified until after we wait on
    // the futures.
  } else {
    // Synchronously clear the segments and overflows.
    for (const auto& seg_to_rewrite : segments_to_rewrite) {
      const SegmentId seg_id = seg_to_rewrite.second->id();
      WritePage(seg_id, 0, zero);
      free_.Add(seg_id);
    }
    for (const auto& overflow_to_clear : overflows_to_clear) {
      WritePage(overflow_to_clear, 0, zero);
      free_.Add(overflow_to_clear);
    }
  }

  // 4. Update in-memory index with the new segments.
  for (const auto& to_remove : segments_to_rewrite) {
    const auto num_removed = index_.erase(to_remove.first);
    assert(num_removed == 1);
  }
  for (const auto& new_segment : rewritten_segments) {
    index_.insert(new_segment);
  }

  // 5. Wait on any write futures.
  for (auto& f : write_futures) {
    f.get();
  }

  // TODO: Log that the rewrite has finished (this log record does not need to
  // be forced to disk for crash consistency).
}

void Manager::FlattenChain(
    const Key base, const std::vector<Record>::const_iterator addtl_rec_begin,
    const std::vector<Record>::const_iterator addtl_rec_end) {
  auto it = index_.lower_bound(base);
  assert(it != index_.end());
  assert(base == it->first);
  assert(it->second.page_count() == 1);
  const SegmentId main_page_id = it->second.id();

  // Retrieve the upper bound.
  ++it;
  Key upper = std::numeric_limits<Key>::max();
  if (it != index_.end()) {
    upper = it->first;
  }

  // Load the existing page(s).
  PageBuffer buf = PageMemoryAllocator::Allocate(/*num_pages=*/2);
  ReadPage(main_page_id, 0, buf.get());
  pg::Page main(buf.get());
  const SegmentId overflow_page_id = main.GetOverflow();
  if (overflow_page_id.IsValid()) {
    // Read the overflow too.
    ReadPage(overflow_page_id, 0, buf.get() + pg::Page::kSize);
  }
  const PageChain pc =
      main.HasOverflow()
          ? PageChain::WithOverflow(buf.get(), buf.get() + pg::Page::kSize)
          : PageChain::SingleOnly(buf.get());
  PageMergeIterator pmi = pc.GetIterator();

  // Merge the records in the chain with those in memory. If two records have
  // the same key, we prefer the in-memory one (it is a more recent write).
  std::vector<Record> records;
  records.reserve(pmi.RecordsLeft() + (addtl_rec_end - addtl_rec_begin));
  auto rec_it = addtl_rec_begin;
  while (pmi.Valid() && rec_it != addtl_rec_end) {
    const Key pmi_key = key_utils::ExtractHead64(pmi.key());
    if (pmi_key == rec_it->first) {
      records.push_back(*rec_it);
      ++rec_it;
      pmi.Next();
    } else if (pmi_key < rec_it->first) {
      records.emplace_back(pmi_key, pmi.value());
      pmi.Next();
    } else {
      // `pmi_key > rec_it->first`
      records.push_back(*rec_it);
      ++rec_it;
    }
  }
  while (pmi.Valid()) {
    const Key pmi_key = key_utils::ExtractHead64(pmi.key());
    records.emplace_back(pmi_key, pmi.value());
    pmi.Next();
  }
  while (rec_it != addtl_rec_end) {
    records.push_back(*rec_it);
    ++rec_it;
  }

  const uint32_t sequence_number = next_sequence_number_++;

  // TODO: Log that we're running a page chain rewrite (include the sequence
  // number and the segment ID).
  const auto new_pages = LoadIntoNewPages(sequence_number, base, upper,
                                          records.begin(), records.end());

  // Update the free list and mark the old pages as invalid.
  void* const zero = buf.get();
  memset(zero, 0, pg::Page::kSize);
  std::vector<std::future<void>> write_futures;
  if (bg_threads_ != nullptr) {
    write_futures.push_back(bg_threads_->Submit(
        [this, main_page_id, zero]() { WritePage(main_page_id, 0, zero); }));
    free_.Add(main_page_id);
    if (overflow_page_id.IsValid()) {
      write_futures.push_back(
          bg_threads_->Submit([this, overflow_page_id, zero]() {
            WritePage(overflow_page_id, 0, zero);
          }));
      free_.Add(overflow_page_id);
    }

  } else {
    WritePage(main_page_id, 0, buf.get());
    free_.Add(main_page_id);
    if (overflow_page_id.IsValid()) {
      WritePage(overflow_page_id, 0, buf.get());
      free_.Add(overflow_page_id);
    }
  }

  // Remove the old segment info and replace it with the new pages.
  index_.erase(base);
  index_.insert(new_pages.begin(), new_pages.end());

  for (auto& f : write_futures) {
    f.get();
  }
}

}  // namespace pg
}  // namespace llsm
