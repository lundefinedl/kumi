#pragma once

using std::stack;
using std::vector;
using std::pair;

class GraphicsObjectHandle;

template<class Traits, int N>
class IdBufferBase {
public:
  typedef typename Traits::Value T;
  typedef typename Traits::Elem E;
  typedef std::function<void(T)> Deleter;

  enum { Size = N };

  IdBufferBase(const Deleter &deleter) : _deleter(deleter) {
    for (int i = 0; i < Size; ++i)
      Traits::get(_buffer[i]) = 0;
  }

  virtual ~IdBufferBase() {
    if (_deleter) {
      for (int i = 0; i < Size; ++i) {
        T t = Traits::get(_buffer[i]);
        if (t)
          _deleter(t);
      }
    }

    while (!reclaimed_indices.empty())
      reclaimed_indices.pop();
  }

  int find_free_index() {
    if (!reclaimed_indices.empty()) {
      int tmp = reclaimed_indices.top();
      reclaimed_indices.pop();
      return tmp;
    }
    for (int i = 0; i < Size; ++i) {
      if (!Traits::get(_buffer[i]))
        return i;
    }
    LOG_ERROR_LN("No free index found!");
    return -1;
  }

  T &operator[](int idx) {
    KASSERT(idx >= 0 && idx < N);
    return Traits::get(_buffer[idx]);
  }

  const T &operator[](int idx) const {
    KASSERT(idx >= 0 && idx < N);
    return Traits::get(_buffer[idx]);
  }

  void reclaim_index(int idx) {
    _reclaimed_indices.push(idx);
  }

  T get(GraphicsObjectHandle handle) {
    return Traits::get(_buffer[handle.id()]);
  }

  const T get(GraphicsObjectHandle handle) const {
    return Traits::get(_buffer[handle.id()]);
  }

protected:
  Deleter _deleter;
  std::array<E, N> _buffer;
  //E _buffer[N];
  stack<int> reclaimed_indices;
};

template<class Key, class Value>
struct SearchableTraits {

  typedef std::pair<Key, Value> KeyValuePair;
  typedef Value Value;
  typedef Key Key;
  typedef KeyValuePair Elem;

  static Key &get_key(KeyValuePair &kv) {
    return kv.first;
  }

  static Value &get(KeyValuePair &kv) {
    return kv.second;
  }
};

template<class T>
struct SingleTraits {
  typedef T Type;
  typedef T Value;
  typedef T Elem;
  static T& get(Elem& t) {
    return t;
  }
};

template<typename Key, typename Value, int N>
struct SearchableIdBuffer : public IdBufferBase<SearchableTraits<Key, Value>, N> {

  typedef SearchableTraits<Key, Value> Traits;
  typedef IdBufferBase<Traits, N> Parent;

  SearchableIdBuffer(const typename Parent::Deleter &fn_deleter) 
    : Parent(fn_deleter)
  {
  }

  void set_pair(int idx, const typename Traits::Elem &e) {
    KASSERT(idx >= 0 && idx < N);
    _buffer[idx] = e;
    auto &key = Traits::get_key(_buffer[idx]);
    _key_to_idx[key] = idx;
    _idx_to_key[idx] = key;
  }

  int idx_from_token(const typename Traits::Key &key) {
    auto it = _key_to_idx.find(key);
    return it == end(_key_to_idx) ? -1 : it->second;
  }

  int find_free_index() {
    return IdBufferBase<Traits, N>::find_free_index();
  }

  int find_free_index(const typename Traits::Key &key) {
    auto idx = idx_from_token(key);
    return idx != -1 ? idx : IdBufferBase<Traits, N>::find_free_index();
  }

  template<typename R>
  R find(const typename Traits::Key &key, R def) {
    int idx = idx_from_token(key);
    return idx == -1 ? def : Traits::get(_buffer[idx]);
  }

  std::map<typename Traits::Key, int> _key_to_idx;
  std::map<int, typename Traits::Key> _idx_to_key;
};

template<typename T, int N>
struct IdBuffer : IdBufferBase<SingleTraits<T>, N> {
  typedef SingleTraits<T> Traits;
  typedef IdBufferBase<Traits, N> Parent;

  IdBuffer(const typename Parent::Deleter &fn_deleter) 
    : Parent(fn_deleter)
  {
  }
};
