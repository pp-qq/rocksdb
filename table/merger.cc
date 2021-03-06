// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/merger.h"

#include "include/comparator.h"
#include "include/iterator.h"
#include "table/iterator_wrapper.h"

namespace leveldb {

namespace {


/*
 * MergingIterator, 可以认为是个多路归并的迭代器. 可以认为是把 children, n 包含着的迭代器按照 comparator 进行
 * 排序得到一个有序的序列, MergingIterator 就是在这个有序的序列上进行遍历.
 *
 * 在实现上, 我认为要遵守一个不变量 "任何一个时刻, current_->key() 始终是所有 children->key() 中最小的那个".
 * 但是 google 并没有这么做==.
 *
 * 这个版本的 MergingIterator 有很大 bug, 参考最新版 leveldb 实现. MergingIterator 仅使用元素不会重复的情况,
 * 当元素重复时, 就算是最新版 leveldb 实现也存在一个 bug.
 */
class MergingIterator : public Iterator {
 public:
  MergingIterator(const Comparator* comparator, Iterator** children, int n)
      : comparator_(comparator),
        children_(new IteratorWrapper[n]),
        n_(n),
        current_(NULL) {
    for (int i = 0; i < n; i++) {
      children_[i].Set(children[i]);
    }
  }

  virtual ~MergingIterator() {
    delete[] children_;
  }

  virtual bool Valid() const {
    return (current_ != NULL);
  }

  virtual void SeekToFirst() {
    for (int i = 0; i < n_; i++) {
      children_[i].SeekToFirst();
    }
    FindSmallest();
  }

  virtual void SeekToLast() {
    for (int i = 0; i < n_; i++) {
      children_[i].SeekToLast();
    }
    FindLargest();
  }

  virtual void Seek(const Slice& target) {
    for (int i = 0; i < n_; i++) {
      children_[i].Seek(target);
    }
    FindSmallest();
  }

  // QA: Next(), Prev() 实现貌似存在 bug. 已经在 merger-test 分支写好验证程序并验证了, 但是还有一点不确定, 等待
  // 有缘人解答==
  // A: 在元素不重复的情况下, 该 bug 并不存在, 事实上在整个 leveldb 中, 元素都是不重复的.
  virtual void Next() {
    assert(Valid());
    current_->Next();
    FindSmallest();
  }

  virtual void Prev() {
    assert(Valid());
    current_->Prev();
    FindLargest();
  }

  virtual Slice key() const {
    assert(Valid());
    return current_->key();
  }

  virtual Slice value() const {
    assert(Valid());
    return current_->value();
  }

  virtual Status status() const {
    Status status;
    for (int i = 0; i < n_; i++) {
      status = children_[i].status();
      if (!status.ok()) {
        break;
      }
    }
    return status;
  }

 private:
  void FindSmallest();
  void FindLargest();

  // We might want to use a heap in case there are lots of children.
  // For now we use a simple array since we expect a very small number
  // of children in leveldb.
  //
  // Q: 啥意思啊? simple array? 在 MergingIterator() 中不是使用 new 在 heap 上分配的内存么?!
  const Comparator* comparator_;
  IteratorWrapper* children_;
  int n_;
  IteratorWrapper* current_;
};

void MergingIterator::FindSmallest() {
  IteratorWrapper* smallest = NULL;
  for (int i = 0; i < n_; i++) {
    IteratorWrapper* child = &children_[i];
    if (child->Valid()) {
      if (smallest == NULL) {
        smallest = child;
      } else if (comparator_->Compare(child->key(), smallest->key()) < 0) {
        smallest = child;
      }
    }
  }
  current_ = smallest;
}

void MergingIterator::FindLargest() {
  IteratorWrapper* largest = NULL;
  for (int i = n_-1; i >= 0; i--) {
    IteratorWrapper* child = &children_[i];
    if (child->Valid()) {
      if (largest == NULL) {
        largest = child;
      } else if (comparator_->Compare(child->key(), largest->key()) > 0) {
        largest = child;
      }
    }
  }
  current_ = largest;
}
}

Iterator* NewMergingIterator(const Comparator* cmp, Iterator** list, int n) {
  assert(n >= 0);
  if (n == 0) {
    return NewEmptyIterator();
  } else if (n == 1) {
    // 本来我以为这里会 memory leak, 经过一番琢磨之后发现并不会==
    return list[0];
  } else {
    return new MergingIterator(cmp, list, n);
  }
}

}
