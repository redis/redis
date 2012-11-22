/*
** hash.c - Hash class
**
** See Copyright Notice in mruby.h
*/

#include "mruby.h"
#include "mruby/hash.h"
#include "mruby/khash.h"
#include "mruby/class.h"
#include "mruby/array.h"
#include "mruby/string.h"
#include "mruby/variable.h"

static inline khint_t
mrb_hash_ht_hash_func(mrb_state *mrb, mrb_value key)
{
  khint_t h = mrb_type(key) << 24;
  mrb_value h2;

  h2 = mrb_funcall(mrb, key, "hash", 0, 0);
  h ^= h2.value.i;
  return h;
}

static inline khint_t
mrb_hash_ht_hash_equal(mrb_state *mrb, mrb_value a, mrb_value b)
{
  return mrb_eql(mrb, a, b);
}

KHASH_DECLARE(ht, mrb_value, mrb_value, 1)
KHASH_DEFINE (ht, mrb_value, mrb_value, 1, mrb_hash_ht_hash_func, mrb_hash_ht_hash_equal)

static void mrb_hash_modify(mrb_state *mrb, mrb_value hash);

static inline mrb_value
mrb_hash_ht_key(mrb_state *mrb, mrb_value key)
{
  if (mrb_string_p(key))
    return mrb_str_dup(mrb, key);
  else
    return key;
}

#define KEY(key) mrb_hash_ht_key(mrb, key)

void
mrb_gc_mark_ht(mrb_state *mrb, struct RHash *hash)
{
  khiter_t k;
  khash_t(ht) *h = hash->ht;

  if (!h) return;
  for (k = kh_begin(h); k != kh_end(h); k++)
    if (kh_exist(h, k)) {
      mrb_gc_mark_value(mrb, kh_key(h, k));
      mrb_gc_mark_value(mrb, kh_value(h, k));
    }
}

size_t
mrb_gc_mark_ht_size(mrb_state *mrb, struct RHash *hash)
{
  if (!hash->ht) return 0;
  return kh_size(hash->ht)*2;
}

void
mrb_gc_free_ht(mrb_state *mrb, struct RHash *hash)
{
  if (hash->ht) kh_destroy(ht, hash->ht);
}


mrb_value
mrb_hash_new_capa(mrb_state *mrb, int capa)
{
  struct RHash *h;

  h = (struct RHash*)mrb_obj_alloc(mrb, MRB_TT_HASH, mrb->hash_class);
  h->ht = kh_init(ht, mrb);
  if (capa > 0) {
    kh_resize(ht, h->ht, capa);
  }
  h->iv = 0;
  return mrb_obj_value(h);
}

mrb_value
mrb_hash_new(mrb_state *mrb)
{
  return mrb_hash_new_capa(mrb, 0);
}

mrb_value
mrb_hash_get(mrb_state *mrb, mrb_value hash, mrb_value key)
{
  khash_t(ht) *h = RHASH_TBL(hash);
  khiter_t k;

  if (h) {
    k = kh_get(ht, h, key);
    if (k != kh_end(h))
      return kh_value(h, k);
  }

  /* not found */
  if (MRB_RHASH_PROCDEFAULT_P(hash)) {
    return mrb_funcall(mrb, RHASH_PROCDEFAULT(hash), "call", 2, hash, key);
  }
  return RHASH_IFNONE(hash);
}

mrb_value
mrb_hash_fetch(mrb_state *mrb, mrb_value hash, mrb_value key, mrb_value def)
{
  khash_t(ht) *h = RHASH_TBL(hash);
  khiter_t k;

  if (h) {
    k = kh_get(ht, h, key);
    if (k != kh_end(h))
      return kh_value(h, k);
  }

  /* not found */
  return def;
}

void
mrb_hash_set(mrb_state *mrb, mrb_value hash, mrb_value key, mrb_value val) /* mrb_hash_aset */
{
  khash_t(ht) *h;
  khiter_t k;

  mrb_hash_modify(mrb, hash);
  h = RHASH_TBL(hash);

  if (!h) h = RHASH_TBL(hash) = kh_init(ht, mrb);
  k = kh_get(ht, h, key);
  if (k == kh_end(h)) {
    /* expand */
    k = kh_put(ht, h, KEY(key));
  }

  kh_value(h, k) = val;
  mrb_write_barrier(mrb, (struct RBasic*)RHASH(hash));
  return;
}

mrb_value
mrb_hash_freeze(mrb_value hash)
{
  //return mrb_obj_freeze(hash);
  return (hash);
}

mrb_value
mrb_hash_dup(mrb_state *mrb, mrb_value hash)
{
  struct RHash* ret;
  khash_t(ht) *h, *ret_h;
  khiter_t k, ret_k;

  h = RHASH_TBL(hash);
  ret = (struct RHash*)mrb_obj_alloc(mrb, MRB_TT_HASH, mrb->hash_class);
  ret->ht = kh_init(ht, mrb);

  if (kh_size(h) > 0) {
    ret_h = ret->ht;

    for (k = kh_begin(h); k != kh_end(h); k++) {
      if (kh_exist(h,k)) {
        ret_k = kh_put(ht, ret_h, KEY(kh_key(h,k)));
        kh_val(ret_h, ret_k) = kh_val(h,k);
      }
    }
  }

  return mrb_obj_value(ret);
}

static void
mrb_hash_modify_check(mrb_state *mrb, mrb_value hash)
{
  //if (OBJ_FROZEN(hash)) mrb_error_frozen("hash");
}

khash_t(ht) *
mrb_hash_tbl(mrb_state *mrb, mrb_value hash)
{
  khash_t(ht) *h = RHASH_TBL(hash);

  if (!h) {
    RHASH_TBL(hash) = kh_init(ht, mrb);
  }
  return h;
}

static void
mrb_hash_modify(mrb_state *mrb, mrb_value hash)
{
  //mrb_hash_modify_check(mrb, hash);
  mrb_hash_tbl(mrb, hash);
}

/* 15.2.13.4.16 */
/*
 *  call-seq:
 *     Hash.new                          -> new_hash
 *     Hash.new(obj)                     -> new_hash
 *     Hash.new {|hash, key| block }     -> new_hash
 *
 *  Returns a new, empty hash. If this hash is subsequently accessed by
 *  a key that doesn't correspond to a hash entry, the value returned
 *  depends on the style of <code>new</code> used to create the hash. In
 *  the first form, the access returns <code>nil</code>. If
 *  <i>obj</i> is specified, this single object will be used for
 *  all <em>default values</em>. If a block is specified, it will be
 *  called with the hash object and the key, and should return the
 *  default value. It is the block's responsibility to store the value
 *  in the hash if required.
 *
 *     h = Hash.new("Go Fish")
 *     h["a"] = 100
 *     h["b"] = 200
 *     h["a"]           #=> 100
 *     h["c"]           #=> "Go Fish"
 *     # The following alters the single default object
 *     h["c"].upcase!   #=> "GO FISH"
 *     h["d"]           #=> "GO FISH"
 *     h.keys           #=> ["a", "b"]
 *
 *     # While this creates a new default object each time
 *     h = Hash.new { |hash, key| hash[key] = "Go Fish: #{key}" }
 *     h["c"]           #=> "Go Fish: c"
 *     h["c"].upcase!   #=> "GO FISH: C"
 *     h["d"]           #=> "Go Fish: d"
 *     h.keys           #=> ["c", "d"]
 *
 */

static mrb_value
mrb_hash_init_core(mrb_state *mrb, mrb_value hash)
{
  mrb_value block, ifnone;
  mrb_value *argv;
  int argc;

  mrb_get_args(mrb, "o*", &block, &argv, &argc);
  mrb_hash_modify(mrb, hash);
  if (mrb_nil_p(block)) {
    if (argc > 0) {
      if (argc != 1) mrb_raise(mrb, E_ARGUMENT_ERROR, "wrong number of arguments");
      ifnone = argv[0];
    }
    else {
      ifnone = mrb_nil_value();
    }
  }
  else {
    if (argc > 0) {
      mrb_raise(mrb, E_ARGUMENT_ERROR, "wrong number of arguments");
    }
    RHASH(hash)->flags |= MRB_HASH_PROC_DEFAULT;
    ifnone = block;
  }
  mrb_iv_set(mrb, hash, mrb_intern(mrb, "ifnone"), ifnone);
  return hash;
}

/*
 *  call-seq:
 *     Hash[ key, value, ... ]         -> new_hash
 *     Hash[ [ [key, value], ... ] ]   -> new_hash
 *     Hash[ object ]                  -> new_hash
 *
 *  Creates a new hash populated with the given objects. Equivalent to
 *  the literal <code>{ <i>key</i> => <i>value</i>, ... }</code>. In the first
 *  form, keys and values occur in pairs, so there must be an even number of arguments.
 *  The second and third form take a single argument which is either
 *  an array of key-value pairs or an object convertible to a hash.
 *
 *     Hash["a", 100, "b", 200]             #=> {"a"=>100, "b"=>200}
 *     Hash[ [ ["a", 100], ["b", 200] ] ]   #=> {"a"=>100, "b"=>200}
 *     Hash["a" => 100, "b" => 200]         #=> {"a"=>100, "b"=>200}
 */

static mrb_value
to_hash(mrb_state *mrb, mrb_value hash)
{
  return mrb_convert_type(mrb, hash, MRB_TT_HASH, "Hash", "to_hash");
}

/*
 *  call-seq:
 *     Hash.try_convert(obj) -> hash or nil
 *
 *  Try to convert <i>obj</i> into a hash, using to_hash method.
 *  Returns converted hash or nil if <i>obj</i> cannot be converted
 *  for any reason.
 *
 *     Hash.try_convert({1=>2})   # => {1=>2}
 *     Hash.try_convert("1=>2")   # => nil
 */

/* 15.2.13.4.2  */
/*
 *  call-seq:
 *     hsh[key]    ->  value
 *
 *  Element Reference---Retrieves the <i>value</i> object corresponding
 *  to the <i>key</i> object. If not found, returns the default value (see
 *  <code>Hash::new</code> for details).
 *
 *     h = { "a" => 100, "b" => 200 }
 *     h["a"]   #=> 100
 *     h["c"]   #=> nil
 *
 */
mrb_value
mrb_hash_aget(mrb_state *mrb, mrb_value self)
{
  mrb_value key;

  mrb_get_args(mrb, "o", &key);
  return mrb_hash_get(mrb, self, key);
}

mrb_value
mrb_hash_lookup(mrb_state *mrb, mrb_value hash, mrb_value key)
{
  return mrb_hash_get(mrb, hash, key);
}

/*
 *  call-seq:
 *     hsh.fetch(key [, default] )       -> obj
 *     hsh.fetch(key) {| key | block }   -> obj
 *
 *  Returns a value from the hash for the given key. If the key can't be
 *  found, there are several options: With no other arguments, it will
 *  raise an <code>KeyError</code> exception; if <i>default</i> is
 *  given, then that will be returned; if the optional code block is
 *  specified, then that will be run and its result returned.
 *
 *     h = { "a" => 100, "b" => 200 }
 *     h.fetch("a")                            #=> 100
 *     h.fetch("z", "go fish")                 #=> "go fish"
 *     h.fetch("z") { |el| "go fish, #{el}"}   #=> "go fish, z"
 *
 *  The following example shows that an exception is raised if the key
 *  is not found and a default value is not supplied.
 *
 *     h = { "a" => 100, "b" => 200 }
 *     h.fetch("z")
 *
 *  <em>produces:</em>
 *
 *     prog.rb:2:in `fetch': key not found (KeyError)
 *      from prog.rb:2
 *
 */

/* 15.2.13.4.5  */
/*
 *  call-seq:
 *     hsh.default(key=nil)   -> obj
 *
 *  Returns the default value, the value that would be returned by
 *  <i>hsh</i>[<i>key</i>] if <i>key</i> did not exist in <i>hsh</i>.
 *  See also <code>Hash::new</code> and <code>Hash#default=</code>.
 *
 *     h = Hash.new                            #=> {}
 *     h.default                               #=> nil
 *     h.default(2)                            #=> nil
 *
 *     h = Hash.new("cat")                     #=> {}
 *     h.default                               #=> "cat"
 *     h.default(2)                            #=> "cat"
 *
 *     h = Hash.new {|h,k| h[k] = k.to_i*10}   #=> {}
 *     h.default                               #=> nil
 *     h.default(2)                            #=> 20
 */

static mrb_value
mrb_hash_default(mrb_state *mrb, mrb_value hash)
{
  mrb_value *argv;
  int argc;
  mrb_value key;

  mrb_get_args(mrb, "*", &argv, &argc);
  if (MRB_RHASH_PROCDEFAULT_P(hash)) {
    if (argc == 0) return mrb_nil_value();
    key = argv[0];
    return mrb_funcall(mrb, RHASH_PROCDEFAULT(hash), "call", 2, hash, key);
  }
  else {
    return RHASH_IFNONE(hash);
  }
}

/* 15.2.13.4.6  */
/*
 *  call-seq:
 *     hsh.default = obj     -> obj
 *
 *  Sets the default value, the value returned for a key that does not
 *  exist in the hash. It is not possible to set the default to a
 *  <code>Proc</code> that will be executed on each key lookup.
 *
 *     h = { "a" => 100, "b" => 200 }
 *     h.default = "Go fish"
 *     h["a"]     #=> 100
 *     h["z"]     #=> "Go fish"
 *     # This doesn't do what you might hope...
 *     h.default = proc do |hash, key|
 *       hash[key] = key + key
 *     end
 *     h[2]       #=> #<Proc:0x401b3948@-:6>
 *     h["cat"]   #=> #<Proc:0x401b3948@-:6>
 */

static mrb_value
mrb_hash_set_default(mrb_state *mrb, mrb_value hash)
{
  mrb_value ifnone;

  mrb_get_args(mrb, "o", &ifnone);
  mrb_hash_modify(mrb, hash);
  mrb_iv_set(mrb, hash, mrb_intern(mrb, "ifnone"), ifnone);
  RHASH(hash)->flags &= ~(MRB_HASH_PROC_DEFAULT);

  return ifnone;
}

/* 15.2.13.4.7  */
/*
 *  call-seq:
 *     hsh.default_proc -> anObject
 *
 *  If <code>Hash::new</code> was invoked with a block, return that
 *  block, otherwise return <code>nil</code>.
 *
 *     h = Hash.new {|h,k| h[k] = k*k }   #=> {}
 *     p = h.default_proc                 #=> #<Proc:0x401b3d08@-:1>
 *     a = []                             #=> []
 *     p.call(a, 2)
 *     a                                  #=> [nil, nil, 4]
 */


static mrb_value
mrb_hash_default_proc(mrb_state *mrb, mrb_value hash)
{
  if (MRB_RHASH_PROCDEFAULT_P(hash)) {
    return RHASH_PROCDEFAULT(hash);
  }
  return mrb_nil_value();
}

/*
 *  call-seq:
 *     hsh.default_proc = proc_obj     -> proc_obj
 *
 *  Sets the default proc to be executed on each key lookup.
 *
 *     h.default_proc = proc do |hash, key|
 *       hash[key] = key + key
 *     end
 *     h[2]       #=> 4
 *     h["cat"]   #=> "catcat"
 */

static mrb_value
mrb_hash_set_default_proc(mrb_state *mrb, mrb_value hash)
{
  mrb_value ifnone;

  mrb_get_args(mrb, "o", &ifnone);
  mrb_hash_modify(mrb, hash);
  mrb_iv_set(mrb, hash, mrb_intern(mrb, "ifnone"), ifnone);
  RHASH(hash)->flags |= MRB_HASH_PROC_DEFAULT;

  return ifnone;
}

mrb_value
mrb_hash_delete_key(mrb_state *mrb, mrb_value hash, mrb_value key)
{
  khash_t(ht) *h = RHASH_TBL(hash);
  khiter_t k;
  mrb_value delVal;

  if (h) {
    k = kh_get(ht, h, key);
    if (k != kh_end(h)) {
      delVal = kh_value(h, k);
      kh_del(ht, h, k);
      return delVal;
    }
  }

  /* not found */
  return mrb_nil_value();
}

/* 15.2.13.4.8  */
/*
 *  call-seq:
 *     hsh.delete(key)                   -> value
 *     hsh.delete(key) {| key | block }  -> value
 *
 *  Deletes and returns a key-value pair from <i>hsh</i> whose key is
 *  equal to <i>key</i>. If the key is not found, returns the
 *  <em>default value</em>. If the optional code block is given and the
 *  key is not found, pass in the key and return the result of
 *  <i>block</i>.
 *
 *     h = { "a" => 100, "b" => 200 }
 *     h.delete("a")                              #=> 100
 *     h.delete("z")                              #=> nil
 *     h.delete("z") { |el| "#{el} not found" }   #=> "z not found"
 *
 */
mrb_value
mrb_hash_delete(mrb_state *mrb, mrb_value self)
{
  mrb_value key;

  mrb_get_args(mrb, "o", &key);
  return mrb_hash_delete_key(mrb, self, key);
}
struct shift_var {
    mrb_value key;
    mrb_value val;
};


/* 15.2.13.4.24 */
/*
 *  call-seq:
 *     hsh.shift -> anArray or obj
 *
 *  Removes a key-value pair from <i>hsh</i> and returns it as the
 *  two-item array <code>[</code> <i>key, value</i> <code>]</code>, or
 *  the hash's default value if the hash is empty.
 *
 *     h = { 1 => "a", 2 => "b", 3 => "c" }
 *     h.shift   #=> [1, "a"]
 *     h         #=> {2=>"b", 3=>"c"}
 */

static mrb_value
mrb_hash_shift(mrb_state *mrb, mrb_value hash)
{
  khash_t(ht) *h = RHASH_TBL(hash);
  khiter_t k;
  mrb_value delKey, delVal;

  mrb_hash_modify(mrb, hash);
  if (h) {
    if (kh_size(h) > 0) {
      for (k = kh_begin(h); k != kh_end(h); k++) {
        if (!kh_exist(h,k)) continue;

        delKey = kh_key(h,k);
        mrb_gc_protect(mrb, delKey);
        delVal = mrb_hash_delete_key(mrb, hash, delKey);
        mrb_gc_protect(mrb, delVal);

	return mrb_assoc_new(mrb, delKey, delVal);
      }
    }
  }

  if (MRB_RHASH_PROCDEFAULT_P(hash)) {
    return mrb_funcall(mrb, RHASH_PROCDEFAULT(hash), "call", 2, hash, mrb_nil_value());
  }
  else {
    return RHASH_IFNONE(hash);
  }
}

/*
 *  call-seq:
 *     hsh.delete_if {| key, value | block }  -> hsh
 *     hsh.delete_if                          -> an_enumerator
 *
 *  Deletes every key-value pair from <i>hsh</i> for which <i>block</i>
 *  evaluates to <code>true</code>.
 *
 *  If no block is given, an enumerator is returned instead.
 *
 *     h = { "a" => 100, "b" => 200, "c" => 300 }
 *     h.delete_if {|key, value| key >= "b" }   #=> {"a"=>100}
 *
 */

/*
 *  call-seq:
 *     hsh.reject! {| key, value | block }  -> hsh or nil
 *     hsh.reject!                          -> an_enumerator
 *
 *  Equivalent to <code>Hash#delete_if</code>, but returns
 *  <code>nil</code> if no changes were made.
 */

/*
 *  call-seq:
 *     hsh.reject {| key, value | block }  -> a_hash
 *
 *  Same as <code>Hash#delete_if</code>, but works on (and returns) a
 *  copy of the <i>hsh</i>. Equivalent to
 *  <code><i>hsh</i>.dup.delete_if</code>.
 *
 */

/*
 * call-seq:
 *   hsh.values_at(key, ...)   -> array
 *
 * Return an array containing the values associated with the given keys.
 * Also see <code>Hash.select</code>.
 *
 *   h = { "cat" => "feline", "dog" => "canine", "cow" => "bovine" }
 *   h.values_at("cow", "cat")  #=> ["bovine", "feline"]
 */

mrb_value
mrb_hash_values_at(mrb_state *mrb, int argc, mrb_value *argv, mrb_value hash)
{
    mrb_value result = mrb_ary_new_capa(mrb, argc);
    long i;

    for (i=0; i<argc; i++) {
        mrb_ary_push(mrb, result, mrb_hash_get(mrb, hash, argv[i]));
    }
    return result;
}

/*
 *  call-seq:
 *     hsh.select {|key, value| block}   -> a_hash
 *     hsh.select                        -> an_enumerator
 *
 *  Returns a new hash consisting of entries for which the block returns true.
 *
 *  If no block is given, an enumerator is returned instead.
 *
 *     h = { "a" => 100, "b" => 200, "c" => 300 }
 *     h.select {|k,v| k > "a"}  #=> {"b" => 200, "c" => 300}
 *     h.select {|k,v| v < 200}  #=> {"a" => 100}
 */

/*
 *  call-seq:
 *     hsh.select! {| key, value | block }  -> hsh or nil
 *     hsh.select!                          -> an_enumerator
 *
 *  Equivalent to <code>Hash#keep_if</code>, but returns
 *  <code>nil</code> if no changes were made.
 */

/*
 *  call-seq:
 *     hsh.keep_if {| key, value | block }  -> hsh
 *     hsh.keep_if                          -> an_enumerator
 *
 *  Deletes every key-value pair from <i>hsh</i> for which <i>block</i>
 *  evaluates to false.
 *
 *  If no block is given, an enumerator is returned instead.
 *
 */

/* 15.2.13.4.4  */
/*
 *  call-seq:
 *     hsh.clear -> hsh
 *
 *  Removes all key-value pairs from <i>hsh</i>.
 *
 *     h = { "a" => 100, "b" => 200 }   #=> {"a"=>100, "b"=>200}
 *     h.clear                          #=> {}
 *
 */

static mrb_value
mrb_hash_clear(mrb_state *mrb, mrb_value hash)
{
  khash_t(ht) *h = RHASH_TBL(hash);

  if (h) kh_clear(ht, h);
  return hash;
}

/* 15.2.13.4.3  */
/* 15.2.13.4.26 */
/*
 *  call-seq:
 *     hsh[key] = value        -> value
 *     hsh.store(key, value)   -> value
 *
 *  Element Assignment---Associates the value given by
 *  <i>value</i> with the key given by <i>key</i>.
 *  <i>key</i> should not have its value changed while it is in
 *  use as a key (a <code>String</code> passed as a key will be
 *  duplicated and frozen).
 *
 *     h = { "a" => 100, "b" => 200 }
 *     h["a"] = 9
 *     h["c"] = 4
 *     h   #=> {"a"=>9, "b"=>200, "c"=>4}
 *
 */
mrb_value
mrb_hash_aset(mrb_state *mrb, mrb_value self)
{
  mrb_value key, val;

  mrb_get_args(mrb, "oo", &key, &val);
  mrb_hash_set(mrb, self, key, val);
  return val;
}

/* 15.2.13.4.17 */
/* 15.2.13.4.23 */
/*
 *  call-seq:
 *     hsh.replace(other_hash) -> hsh
 *
 *  Replaces the contents of <i>hsh</i> with the contents of
 *  <i>other_hash</i>.
 *
 *     h = { "a" => 100, "b" => 200 }
 *     h.replace({ "c" => 300, "d" => 400 })   #=> {"c"=>300, "d"=>400}
 *
 */

static mrb_value
mrb_hash_replace(mrb_state *mrb, mrb_value hash)
{
  mrb_value hash2, ifnone;
  khash_t(ht) *h2;
  khiter_t k;

  mrb_get_args(mrb, "o", &hash2);
  mrb_hash_modify_check(mrb, hash);
  hash2 = to_hash(mrb, hash2);
  if (mrb_obj_equal(mrb, hash, hash2)) return hash;
  mrb_hash_clear(mrb, hash);

  h2 = RHASH_TBL(hash2);
  if (h2) {
    for (k = kh_begin(h2); k != kh_end(h2); k++) {
      if (kh_exist(h2, k))
        mrb_hash_set(mrb, hash, kh_key(h2, k), kh_value(h2, k));
    }
  }

  if (MRB_RHASH_PROCDEFAULT_P(hash2)) {
    RHASH(hash)->flags |= MRB_HASH_PROC_DEFAULT;
    ifnone = RHASH_PROCDEFAULT(hash2);
  }
  else {
    ifnone = RHASH_IFNONE(hash2);
  }
  mrb_iv_set(mrb, hash, mrb_intern(mrb, "ifnone"), ifnone);

  return hash;
}

/* 15.2.13.4.20 */
/* 15.2.13.4.25 */
/*
 *  call-seq:
 *     hsh.length    ->  fixnum
 *     hsh.size      ->  fixnum
 *
 *  Returns the number of key-value pairs in the hash.
 *
 *     h = { "d" => 100, "a" => 200, "v" => 300, "e" => 400 }
 *     h.length        #=> 4
 *     h.delete("a")   #=> 200
 *     h.length        #=> 3
 */
static mrb_value
mrb_hash_size_m(mrb_state *mrb, mrb_value self)
{
  khash_t(ht) *h = RHASH_TBL(self);

  if (!h) return mrb_fixnum_value(0);
  return mrb_fixnum_value(kh_size(h));
}

/* 15.2.13.4.12 */
/*
 *  call-seq:
 *     hsh.empty?    -> true or false
 *
 *  Returns <code>true</code> if <i>hsh</i> contains no key-value pairs.
 *
 *     {}.empty?   #=> true
 *
 */
static mrb_value
mrb_hash_empty_p(mrb_state *mrb, mrb_value self)
{
  khash_t(ht) *h = RHASH_TBL(self);

  if (h) {
    if (kh_size(h) == 0)
      return mrb_true_value();
  }
  return mrb_false_value();
}

/* 15.2.13.4.11 */
/*
 *  call-seq:
 *     hsh.each_value {| value | block } -> hsh
 *     hsh.each_value                    -> an_enumerator
 *
 *  Calls <i>block</i> once for each key in <i>hsh</i>, passing the
 *  value as a parameter.
 *
 *  If no block is given, an enumerator is returned instead.
 *
 *     h = { "a" => 100, "b" => 200 }
 *     h.each_value {|value| puts value }
 *
 *  <em>produces:</em>
 *
 *     100
 *     200
 */

/* 15.2.13.4.10 */
/*
 *  call-seq:
 *     hsh.each_key {| key | block } -> hsh
 *     hsh.each_key                  -> an_enumerator
 *
 *  Calls <i>block</i> once for each key in <i>hsh</i>, passing the key
 *  as a parameter.
 *
 *  If no block is given, an enumerator is returned instead.
 *
 *     h = { "a" => 100, "b" => 200 }
 *     h.each_key {|key| puts key }
 *
 *  <em>produces:</em>
 *
 *     a
 *     b
 */

/* 15.2.13.4.9  */
/*
 *  call-seq:
 *     hsh.each      {| key, value | block } -> hsh
 *     hsh.each_pair {| key, value | block } -> hsh
 *     hsh.each                              -> an_enumerator
 *     hsh.each_pair                         -> an_enumerator
 *
 *  Calls <i>block</i> once for each key in <i>hsh</i>, passing the key-value
 *  pair as parameters.
 *
 *  If no block is given, an enumerator is returned instead.
 *
 *     h = { "a" => 100, "b" => 200 }
 *     h.each {|key, value| puts "#{key} is #{value}" }
 *
 *  <em>produces:</em>
 *
 *     a is 100
 *     b is 200
 *
 */

static mrb_value
inspect_hash(mrb_state *mrb, mrb_value hash, int recur)
{
  mrb_value str, str2;
  khash_t(ht) *h = RHASH_TBL(hash);
  khiter_t k;

  if (recur) return mrb_str_new(mrb, "{...}", 5);

  str = mrb_str_new(mrb, "{", 1);
  if (h && kh_size(h) > 0) {
    for (k = kh_begin(h); k != kh_end(h); k++) {
      int ai;

      if (!kh_exist(h,k)) continue;

      ai = mrb_gc_arena_save(mrb);

      if (RSTRING_LEN(str) > 1) mrb_str_cat2(mrb, str, ", ");

      str2 = mrb_inspect(mrb, kh_key(h,k));
      mrb_str_append(mrb, str, str2);
      mrb_str_buf_cat(mrb, str, "=>", 2);
      str2 = mrb_inspect(mrb, kh_value(h,k));
      mrb_str_append(mrb, str, str2);

      mrb_gc_arena_restore(mrb, ai);
    }
  }
  mrb_str_buf_cat(mrb, str, "}", 1);

  return str;
}

/* 15.2.13.4.30 (x)*/
/*
 * call-seq:
 *   hsh.to_s     -> string
 *   hsh.inspect  -> string
 *
 * Return the contents of this hash as a string.
 *
 *     h = { "c" => 300, "a" => 100, "d" => 400, "c" => 300  }
 *     h.to_s   #=> "{\"c\"=>300, \"a\"=>100, \"d\"=>400}"
 */

static mrb_value
mrb_hash_inspect(mrb_state *mrb, mrb_value hash)
{
  khash_t(ht) *h = RHASH_TBL(hash);

  if (!h || kh_size(h) == 0)
    return mrb_str_new(mrb, "{}", 2);
  return inspect_hash(mrb, hash, 0);
}

/* 15.2.13.4.29 (x)*/
/*
 * call-seq:
 *    hsh.to_hash   => hsh
 *
 * Returns +self+.
 */

static mrb_value
mrb_hash_to_hash(mrb_state *mrb, mrb_value hash)
{
    return hash;
}

/* 15.2.13.4.19 */
/*
 *  call-seq:
 *     hsh.keys    -> array
 *
 *  Returns a new array populated with the keys from this hash. See also
 *  <code>Hash#values</code>.
 *
 *     h = { "a" => 100, "b" => 200, "c" => 300, "d" => 400 }
 *     h.keys   #=> ["a", "b", "c", "d"]
 *
 */

mrb_value
mrb_hash_keys(mrb_state *mrb, mrb_value hash)
{
  khash_t(ht) *h = RHASH_TBL(hash);
  khiter_t k;
  mrb_value ary;

  if (!h) return mrb_ary_new(mrb);
  ary = mrb_ary_new_capa(mrb, kh_size(h));
  for (k = kh_begin(h); k != kh_end(h); k++) {
    if (kh_exist(h, k)) {
      mrb_value v = kh_key(h,k);
      mrb_ary_push(mrb, ary, v);
    }
  }
  return ary;
}

/* 15.2.13.4.28 */
/*
 *  call-seq:
 *     hsh.values    -> array
 *
 *  Returns a new array populated with the values from <i>hsh</i>. See
 *  also <code>Hash#keys</code>.
 *
 *     h = { "a" => 100, "b" => 200, "c" => 300 }
 *     h.values   #=> [100, 200, 300]
 *
 */

static mrb_value
mrb_hash_values(mrb_state *mrb, mrb_value hash)
{
  khash_t(ht) *h = RHASH_TBL(hash);
  khiter_t k;
  mrb_value ary;

  if (!h) return mrb_ary_new(mrb);
  ary = mrb_ary_new_capa(mrb, kh_size(h));
  for (k = kh_begin(h); k != kh_end(h); k++) {
    if (kh_exist(h, k)){
      mrb_value v = kh_value(h,k);
      mrb_ary_push(mrb, ary, v);
    }
  }
  return ary;
}

static mrb_value
mrb_hash_has_keyWithKey(mrb_state *mrb, mrb_value hash, mrb_value key)
{
  khash_t(ht) *h = RHASH_TBL(hash);
  khiter_t k;

  if (h) {
    k = kh_get(ht, h, key);
    if (k != kh_end(h))
      return mrb_true_value();
  }

  return mrb_false_value();
}

/* 15.2.13.4.13 */
/* 15.2.13.4.15 */
/* 15.2.13.4.18 */
/* 15.2.13.4.21 */
/*
 *  call-seq:
 *     hsh.has_key?(key)    -> true or false
 *     hsh.include?(key)    -> true or false
 *     hsh.key?(key)        -> true or false
 *     hsh.member?(key)     -> true or false
 *
 *  Returns <code>true</code> if the given key is present in <i>hsh</i>.
 *
 *     h = { "a" => 100, "b" => 200 }
 *     h.has_key?("a")   #=> true
 *     h.has_key?("z")   #=> false
 *
 */

static mrb_value
mrb_hash_has_key(mrb_state *mrb, mrb_value hash)
{
  mrb_value key;

  mrb_get_args(mrb, "o", &key);
  return mrb_hash_has_keyWithKey(mrb, hash, key);
}

static mrb_value
mrb_hash_has_valueWithvalue(mrb_state *mrb, mrb_value hash, mrb_value value)
{
  khash_t(ht) *h = RHASH_TBL(hash);
  khiter_t k;

  if (h) {
    for (k = kh_begin(h); k != kh_end(h); k++) {
      if (!kh_exist(h, k)) continue;

      if (mrb_equal(mrb, kh_value(h,k), value)) {
        return mrb_true_value();
      }
    }
  }

  return mrb_false_value();
}

/* 15.2.13.4.14 */
/* 15.2.13.4.27 */
/*
 *  call-seq:
 *     hsh.has_value?(value)    -> true or false
 *     hsh.value?(value)        -> true or false
 *
 *  Returns <code>true</code> if the given value is present for some key
 *  in <i>hsh</i>.
 *
 *     h = { "a" => 100, "b" => 200 }
 *     h.has_value?(100)   #=> true
 *     h.has_value?(999)   #=> false
 */

static mrb_value
mrb_hash_has_value(mrb_state *mrb, mrb_value hash)
{
  mrb_value val;

  mrb_get_args(mrb, "o", &val);
  return mrb_hash_has_valueWithvalue(mrb, hash, val);
}

static mrb_value
hash_equal(mrb_state *mrb, mrb_value hash1, mrb_value hash2, int eql)
{
  khash_t(ht) *h1, *h2;

  if (mrb_obj_equal(mrb, hash1, hash2)) return mrb_true_value();
  if (!mrb_hash_p(hash2)) {
      if (!mrb_respond_to(mrb, hash2, mrb_intern(mrb, "to_hash"))) {
          return mrb_false_value();
      }
      if (eql)
          return mrb_fixnum_value(mrb_eql(mrb, hash2, hash1));
      else
          return mrb_fixnum_value(mrb_equal(mrb, hash2, hash1));
  }
  h1 = RHASH_TBL(hash1);
  h2 = RHASH_TBL(hash2);
  if (!h2) {
    if (!h2)  return mrb_true_value();
    return mrb_false_value();
  }
  if (!h2) return mrb_false_value();
  if (kh_size(h1) != kh_size(h2)) return mrb_false_value();
  else {
    khiter_t k1, k2;
    mrb_value key;

    for (k1 = kh_begin(h1); k1 != kh_end(h1); k1++) {
      if (!kh_exist(h1, k1)) continue;
      key = kh_key(h1,k1);
      k2 = kh_get(ht, h2, key);
      if (k2 != kh_end(h2)) {
	if (mrb_equal(mrb, kh_value(h1,k1), kh_value(h2,k2))) {
	  continue; /* next key */
	}
      }
      return mrb_false_value();
    }
  }
  return mrb_true_value();
}

/* 15.2.13.4.1  */
/*
 *  call-seq:
 *     hsh == other_hash    -> true or false
 *
 *  Equality---Two hashes are equal if they each contain the same number
 *  of keys and if each key-value pair is equal to (according to
 *  <code>Object#==</code>) the corresponding elements in the other
 *  hash.
 *
 *     h1 = { "a" => 1, "c" => 2 }
 *     h2 = { 7 => 35, "c" => 2, "a" => 1 }
 *     h3 = { "a" => 1, "c" => 2, 7 => 35 }
 *     h4 = { "a" => 1, "d" => 2, "f" => 35 }
 *     h1 == h2   #=> false
 *     h2 == h3   #=> true
 *     h3 == h4   #=> false
 *
 */

static mrb_value
mrb_hash_equal(mrb_state *mrb, mrb_value hash1)
{
  mrb_value hash2;

  mrb_get_args(mrb, "o", &hash2);
  return hash_equal(mrb, hash1, hash2, FALSE);
}

/* 15.2.13.4.32 (x)*/
/*
 *  call-seq:
 *     hash.eql?(other)  -> true or false
 *
 *  Returns <code>true</code> if <i>hash</i> and <i>other</i> are
 *  both hashes with the same content.
 */

static mrb_value
mrb_hash_eql(mrb_state *mrb, mrb_value hash1)
{
  mrb_value hash2;

  mrb_get_args(mrb, "o", &hash2);
  return hash_equal(mrb, hash1, hash2, TRUE);
}

/*
 *  call-seq:
 *     hsh.merge!(other_hash)                                 -> hsh
 *     hsh.update(other_hash)                                 -> hsh
 *     hsh.merge!(other_hash){|key, oldval, newval| block}    -> hsh
 *     hsh.update(other_hash){|key, oldval, newval| block}    -> hsh
 *
 *  Adds the contents of <i>other_hash</i> to <i>hsh</i>.  If no
 *  block is specified, entries with duplicate keys are overwritten
 *  with the values from <i>other_hash</i>, otherwise the value
 *  of each duplicate key is determined by calling the block with
 *  the key, its value in <i>hsh</i> and its value in <i>other_hash</i>.
 *
 *     h1 = { "a" => 100, "b" => 200 }
 *     h2 = { "b" => 254, "c" => 300 }
 *     h1.merge!(h2)   #=> {"a"=>100, "b"=>254, "c"=>300}
 *
 *     h1 = { "a" => 100, "b" => 200 }
 *     h2 = { "b" => 254, "c" => 300 }
 *     h1.merge!(h2) { |key, v1, v2| v1 }
 *                     #=> {"a"=>100, "b"=>200, "c"=>300}
 */

/* 15.2.13.4.22 */
/*
 *  call-seq:
 *     hsh.merge(other_hash)                              -> new_hash
 *     hsh.merge(other_hash){|key, oldval, newval| block} -> new_hash
 *
 *  Returns a new hash containing the contents of <i>other_hash</i> and
 *  the contents of <i>hsh</i>. If no block is specified, the value for
 *  entries with duplicate keys will be that of <i>other_hash</i>. Otherwise
 *  the value for each duplicate key is determined by calling the block
 *  with the key, its value in <i>hsh</i> and its value in <i>other_hash</i>.
 *
 *     h1 = { "a" => 100, "b" => 200 }
 *     h2 = { "b" => 254, "c" => 300 }
 *     h1.merge(h2)   #=> {"a"=>100, "b"=>254, "c"=>300}
 *     h1.merge(h2){|key, oldval, newval| newval - oldval}
 *                    #=> {"a"=>100, "b"=>54,  "c"=>300}
 *     h1             #=> {"a"=>100, "b"=>200}
 *
 */

/*
 *  call-seq:
 *     hash.assoc(obj)   ->  an_array  or  nil
 *
 *  Searches through the hash comparing _obj_ with the key using <code>==</code>.
 *  Returns the key-value pair (two elements array) or +nil+
 *  if no match is found.  See <code>Array#assoc</code>.
 *
 *     h = {"colors"  => ["red", "blue", "green"],
 *          "letters" => ["a", "b", "c" ]}
 *     h.assoc("letters")  #=> ["letters", ["a", "b", "c"]]
 *     h.assoc("foo")      #=> nil
 */

mrb_value
mrb_hash_assoc(mrb_state *mrb, mrb_value hash)
{
  mrb_value key, value, has_key;

  mrb_get_args(mrb, "o", &key);
  if (mrb_nil_p(key))
    mrb_raise(mrb, E_ARGUMENT_ERROR, "wrong number of arguments");

  has_key = mrb_hash_has_keyWithKey(mrb, hash, key);
  if (mrb_test(has_key)) {
    value = mrb_hash_get(mrb, hash, key);
    return mrb_assoc_new(mrb, key, value);
  }
  else {
    return mrb_nil_value();
  }
}

/*
 *  call-seq:
 *     hash.rassoc(key) -> an_array or nil
 *
 *  Searches through the hash comparing _obj_ with the value using <code>==</code>.
 *  Returns the first key-value pair (two-element array) that matches. See
 *  also <code>Array#rassoc</code>.
 *
 *     a = {1=> "one", 2 => "two", 3 => "three", "ii" => "two"}
 *     a.rassoc("two")    #=> [2, "two"]
 *     a.rassoc("four")   #=> nil
 */

mrb_value
mrb_hash_rassoc(mrb_state *mrb, mrb_value hash)
{
  mrb_value key, value, has_key;

  mrb_get_args(mrb, "o", &key);
  has_key = mrb_hash_has_keyWithKey(mrb, hash, key);
  if (mrb_test(has_key)) {
    value = mrb_hash_get(mrb, hash, key);
    return mrb_assoc_new(mrb, value, key);
  }
  else {
    return mrb_nil_value();
  }
}

/*
 *  call-seq:
 *     hash.flatten -> an_array
 *     hash.flatten(level) -> an_array
 *
 *  Returns a new array that is a one-dimensional flattening of this
 *  hash. That is, for every key or value that is an array, extract
 *  its elements into the new array.  Unlike Array#flatten, this
 *  method does not flatten recursively by default.  The optional
 *  <i>level</i> argument determines the level of recursion to flatten.
 *
 *     a =  {1=> "one", 2 => [2,"two"], 3 => "three"}
 *     a.flatten    # => [1, "one", 2, [2, "two"], 3, "three"]
 *     a.flatten(2) # => [1, "one", 2, 2, "two", 3, "three"]
 */

/*
 *  A <code>Hash</code> is a collection of key-value pairs. It is
 *  similar to an <code>Array</code>, except that indexing is done via
 *  arbitrary keys of any object type, not an integer index. Hashes enumerate
 *  their values in the order that the corresponding keys were inserted.
 *
 *  Hashes have a <em>default value</em> that is returned when accessing
 *  keys that do not exist in the hash. By default, that value is
 *  <code>nil</code>.
 *
 */

void
mrb_init_hash(mrb_state *mrb)
{
  struct RClass *h;

  h = mrb->hash_class = mrb_define_class(mrb, "Hash", mrb->object_class);
  MRB_SET_INSTANCE_TT(h, MRB_TT_HASH);

  mrb_include_module(mrb, h, mrb_class_get(mrb, "Enumerable"));
  mrb_define_method(mrb, h, "==",              mrb_hash_equal,       ARGS_REQ(1)); /* 15.2.13.4.1  */
  mrb_define_method(mrb, h, "[]",              mrb_hash_aget,        ARGS_REQ(1)); /* 15.2.13.4.2  */
  mrb_define_method(mrb, h, "[]=",             mrb_hash_aset,        ARGS_REQ(2)); /* 15.2.13.4.3  */
  mrb_define_method(mrb, h, "clear",           mrb_hash_clear,       ARGS_NONE()); /* 15.2.13.4.4  */
  mrb_define_method(mrb, h, "default",         mrb_hash_default,     ARGS_ANY());  /* 15.2.13.4.5  */
  mrb_define_method(mrb, h, "default=",        mrb_hash_set_default, ARGS_REQ(1)); /* 15.2.13.4.6  */
  mrb_define_method(mrb, h, "default_proc",    mrb_hash_default_proc,ARGS_NONE()); /* 15.2.13.4.7  */
  mrb_define_method(mrb, h, "default_proc=",   mrb_hash_set_default_proc,ARGS_REQ(1)); /* 15.2.13.4.7  */
  mrb_define_method(mrb, h, "__delete",        mrb_hash_delete,      ARGS_REQ(1)); /* core of 15.2.13.4.8  */
// "each"                                                                             15.2.13.4.9  move to mrblib/hash.rb
// "each_key"                                                                         15.2.13.4.10 move to mrblib/hash.rb
// "each_value"                                                                       15.2.13.4.11 move to mrblib/hash.rb
  mrb_define_method(mrb, h, "empty?",          mrb_hash_empty_p,     ARGS_NONE()); /* 15.2.13.4.12 */
  mrb_define_method(mrb, h, "has_key?",        mrb_hash_has_key,     ARGS_REQ(1)); /* 15.2.13.4.13 */
  mrb_define_method(mrb, h, "has_value?",      mrb_hash_has_value,   ARGS_REQ(1)); /* 15.2.13.4.14 */
  mrb_define_method(mrb, h, "include?",        mrb_hash_has_key,     ARGS_REQ(1)); /* 15.2.13.4.15 */
  mrb_define_method(mrb, h, "__init_core",     mrb_hash_init_core,   ARGS_ANY());  /* core of 15.2.13.4.16 */
  mrb_define_method(mrb, h, "initialize_copy", mrb_hash_replace,     ARGS_REQ(1)); /* 15.2.13.4.17 */
  mrb_define_method(mrb, h, "key?",            mrb_hash_has_key,     ARGS_REQ(1)); /* 15.2.13.4.18 */
  mrb_define_method(mrb, h, "keys",            mrb_hash_keys,        ARGS_NONE()); /* 15.2.13.4.19 */
  mrb_define_method(mrb, h, "length",          mrb_hash_size_m,      ARGS_NONE()); /* 15.2.13.4.20 */
  mrb_define_method(mrb, h, "member?",         mrb_hash_has_key,     ARGS_REQ(1)); /* 15.2.13.4.21 */
// "merge"                                                                            15.2.13.4.22 move to mrblib/hash.rb
  mrb_define_method(mrb, h, "replace",         mrb_hash_replace,     ARGS_REQ(1)); /* 15.2.13.4.23 */
  mrb_define_method(mrb, h, "shift",           mrb_hash_shift,       ARGS_NONE()); /* 15.2.13.4.24 */
  mrb_define_method(mrb, h, "size",            mrb_hash_size_m,      ARGS_NONE()); /* 15.2.13.4.25 */
  mrb_define_method(mrb, h, "store",           mrb_hash_aset,        ARGS_REQ(2)); /* 15.2.13.4.26 */
  mrb_define_method(mrb, h, "value?",          mrb_hash_has_value,   ARGS_REQ(1)); /* 15.2.13.4.27 */
  mrb_define_method(mrb, h, "values",          mrb_hash_values,      ARGS_NONE()); /* 15.2.13.4.28 */

  mrb_define_method(mrb, h, "to_hash",         mrb_hash_to_hash,     ARGS_NONE()); /* 15.2.13.4.29 (x)*/
  mrb_define_method(mrb, h, "inspect",         mrb_hash_inspect,     ARGS_NONE()); /* 15.2.13.4.30 (x)*/
  mrb_define_alias(mrb,  h, "to_s",            "inspect");                         /* 15.2.13.4.31 (x)*/
  mrb_define_method(mrb, h, "eql?",            mrb_hash_eql,         ARGS_REQ(1)); /* 15.2.13.4.32 (x)*/
}
