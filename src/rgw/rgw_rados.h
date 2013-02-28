#ifndef CEPH_RGWRADOS_H
#define CEPH_RGWRADOS_H

#include "include/rados/librados.hpp"
#include "include/Context.h"
#include "common/RefCountedObj.h"
#include "rgw_common.h"
#include "cls/rgw/cls_rgw_types.h"
#include "cls/version/cls_version_types.h"
#include "rgw_log.h"

class RGWWatcher;
class SafeTimer;
class ACLOwner;
class RGWGC;

/* flags for put_obj_meta() */
#define PUT_OBJ_CREATE      0x01
#define PUT_OBJ_EXCL        0x02
#define PUT_OBJ_CREATE_EXCL (PUT_OBJ_CREATE | PUT_OBJ_EXCL)

#define RGW_OBJ_NS_MULTIPART "multipart"
#define RGW_OBJ_NS_SHADOW    "shadow"

static inline void prepend_bucket_marker(rgw_bucket& bucket, string& orig_oid, string& oid)
{
  if (bucket.marker.empty() || orig_oid.empty()) {
    oid = orig_oid;
  } else {
    oid = bucket.marker;
    oid.append("_");
    oid.append(orig_oid);
  }
}

static inline void get_obj_bucket_and_oid_key(rgw_obj& obj, rgw_bucket& bucket, string& oid, string& key)
{
  bucket = obj.bucket;
  prepend_bucket_marker(bucket, obj.object, oid);
  prepend_bucket_marker(bucket, obj.key, key);
}

struct RGWUsageBatch {
  map<utime_t, rgw_usage_log_entry> m;

  void insert(utime_t& t, rgw_usage_log_entry& entry, bool *account) {
    bool exists = m.find(t) != m.end();
    *account = !exists;
    m[t].aggregate(entry);
  }
};

struct RGWUsageIter {
  string read_iter;
  uint32_t index;

  RGWUsageIter() : index(0) {}
};

class RGWGetDataCB {
public:
  virtual int handle_data(bufferlist& bl, off_t bl_ofs, off_t bl_len) = 0;
  virtual ~RGWGetDataCB() {}
};

class RGWAccessListFilter {
public:
  virtual ~RGWAccessListFilter() {}
  virtual bool filter(string& name, string& key) = 0;
};

struct RGWCloneRangeInfo {
  rgw_obj src;
  off_t src_ofs;
  off_t dst_ofs;
  uint64_t len;
};

struct RGWObjManifestPart {
  rgw_obj loc;       /* the object where the data is located */
  uint64_t loc_ofs;  /* the offset at that object where the data is located */
  uint64_t size;     /* the part size */

  RGWObjManifestPart() : loc_ofs(0), size(0) {}

  void encode(bufferlist& bl) const {
    ENCODE_START(2, 2, bl);
    ::encode(loc, bl);
    ::encode(loc_ofs, bl);
    ::encode(size, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
     DECODE_START_LEGACY_COMPAT_LEN_32(2, 2, 2, bl);
     ::decode(loc, bl);
     ::decode(loc_ofs, bl);
     ::decode(size, bl);
     DECODE_FINISH(bl);
  }

  void dump(Formatter *f) const;
  static void generate_test_instances(list<RGWObjManifestPart*>& o);
};
WRITE_CLASS_ENCODER(RGWObjManifestPart);

struct RGWObjManifest {
  map<uint64_t, RGWObjManifestPart> objs;
  uint64_t obj_size;

  RGWObjManifest() : obj_size(0) {}

  void encode(bufferlist& bl) const {
    ENCODE_START(2, 2, bl);
    ::encode(obj_size, bl);
    ::encode(objs, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
     DECODE_START_LEGACY_COMPAT_LEN_32(2, 2, 2, bl);
     ::decode(obj_size, bl);
     ::decode(objs, bl);
     DECODE_FINISH(bl);
  }

  void dump(Formatter *f) const;
  static void generate_test_instances(list<RGWObjManifest*>& o);

  void append(RGWObjManifest& m);

  bool empty() { return objs.empty(); }
};
WRITE_CLASS_ENCODER(RGWObjManifest);

struct RGWUploadPartInfo {
  uint32_t num;
  uint64_t size;
  string etag;
  utime_t modified;
  RGWObjManifest manifest;

  RGWUploadPartInfo() : num(0), size(0) {}

  void encode(bufferlist& bl) const {
    ENCODE_START(3, 2, bl);
    ::encode(num, bl);
    ::encode(size, bl);
    ::encode(etag, bl);
    ::encode(modified, bl);
    ::encode(manifest, bl);
    ENCODE_FINISH(bl);
  }
  void decode(bufferlist::iterator& bl) {
    DECODE_START_LEGACY_COMPAT_LEN(3, 2, 2, bl);
    ::decode(num, bl);
    ::decode(size, bl);
    ::decode(etag, bl);
    ::decode(modified, bl);
    if (struct_v >= 3)
      ::decode(manifest, bl);
    DECODE_FINISH(bl);
  }
  void dump(Formatter *f) const;
  static void generate_test_instances(list<RGWUploadPartInfo*>& o);
};
WRITE_CLASS_ENCODER(RGWUploadPartInfo)

struct RGWObjState {
  bool is_atomic;
  bool has_attrs;
  bool exists;
  uint64_t size;
  time_t mtime;
  uint64_t epoch;
  bufferlist obj_tag;
  bool fake_tag;
  RGWObjManifest manifest;
  bool has_manifest;
  string shadow_obj;
  bool has_data;
  bufferlist data;
  bool prefetch_data;

  map<string, bufferlist> attrset;
  RGWObjState() : is_atomic(false), has_attrs(0), exists(false),
                  size(0), mtime(0), epoch(0), fake_tag(false), has_manifest(false),
                  has_data(false), prefetch_data(false) {}

  bool get_attr(string name, bufferlist& dest) {
    map<string, bufferlist>::iterator iter = attrset.find(name);
    if (iter != attrset.end()) {
      dest = iter->second;
      return true;
    }
    return false;
  }

  void clear() {
    has_attrs = false;
    exists = false;
    fake_tag = false;
    epoch = 0;
    size = 0;
    mtime = 0;
    obj_tag.clear();
    shadow_obj.clear();
    attrset.clear();
    data.clear();
  }
};

struct RGWRadosCtx {
  RGWRados *store;
  map<rgw_obj, RGWObjState> objs_state;
  int (*intent_cb)(RGWRados *store, void *user_ctx, rgw_obj& obj, RGWIntentEvent intent);
  void *user_ctx;

  RGWRadosCtx(RGWRados *_store) : store(_store), intent_cb(NULL), user_ctx(NULL) { }

  RGWObjState *get_state(rgw_obj& obj);
  void set_atomic(rgw_obj& obj);
  void set_prefetch_data(rgw_obj& obj);

  void set_intent_cb(int (*cb)(RGWRados *store, void *user_ctx, rgw_obj& obj, RGWIntentEvent intent)) {
    intent_cb = cb;
  }

  int notify_intent(RGWRados *store, rgw_obj& obj, RGWIntentEvent intent) {
    if (intent_cb) {
      return intent_cb(store, user_ctx, obj, intent);
    }
    return 0;
  }
};

struct RGWPoolIterCtx {
  librados::IoCtx io_ctx;
  librados::ObjectIterator iter;
};

struct RGWListRawObjsCtx {
  bool initialized;
  RGWPoolIterCtx iter_ctx;

  RGWListRawObjsCtx() : initialized(false) {}
};

struct RGWRegion;

struct RGWZoneParams {
  rgw_bucket domain_root;
  rgw_bucket control_pool;
  rgw_bucket gc_pool;
  rgw_bucket log_pool;
  rgw_bucket intent_log_pool;
  rgw_bucket usage_log_pool;

  rgw_bucket user_keys_pool;
  rgw_bucket user_email_pool;
  rgw_bucket user_swift_pool;
  rgw_bucket user_uid_pool;

  string name;

  static string get_pool_name(CephContext *cct);
  void init_name(CephContext *cct, RGWRegion& region);
  int init(CephContext *cct, RGWRados *store, RGWRegion& region);
  void init_default();
  int store_info(CephContext *cct, RGWRados *store, RGWRegion& region);

  void encode(bufferlist& bl) const {
    ENCODE_START(2, 1, bl);
    ::encode(domain_root, bl);
    ::encode(control_pool, bl);
    ::encode(gc_pool, bl);
    ::encode(log_pool, bl);
    ::encode(intent_log_pool, bl);
    ::encode(usage_log_pool, bl);
    ::encode(user_keys_pool, bl);
    ::encode(user_email_pool, bl);
    ::encode(user_swift_pool, bl);
    ::encode(user_uid_pool, bl);
    ::encode(name, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
     DECODE_START(2, bl);
    ::decode(domain_root, bl);
    ::decode(control_pool, bl);
    ::decode(gc_pool, bl);
    ::decode(log_pool, bl);
    ::decode(intent_log_pool, bl);
    ::decode(usage_log_pool, bl);
    ::decode(user_keys_pool, bl);
    ::decode(user_email_pool, bl);
    ::decode(user_swift_pool, bl);
    ::decode(user_uid_pool, bl);
    if (struct_v >= 2)
      ::decode(name, bl);
    DECODE_FINISH(bl);
  }
  void dump(Formatter *f) const;
  void decode_json(JSONObj *obj);
};
WRITE_CLASS_ENCODER(RGWZoneParams);

struct RGWZone {
  string name;
  list<string> endpoints;

  void encode(bufferlist& bl) const {
    ENCODE_START(1, 1, bl);
    ::encode(name, bl);
    ::encode(endpoints, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
    DECODE_START(1, bl);
    ::decode(name, bl);
    ::decode(endpoints, bl);
    DECODE_FINISH(bl);
  }
  void dump(Formatter *f) const;
  void decode_json(JSONObj *obj);
};
WRITE_CLASS_ENCODER(RGWZone);

struct RGWDefaultRegionInfo {
  string default_region;

  void encode(bufferlist& bl) const {
    ENCODE_START(1, 1, bl);
    ::encode(default_region, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
    DECODE_START(1, bl);
    ::decode(default_region, bl);
    DECODE_FINISH(bl);
  }
  void dump(Formatter *f) const;
  void decode_json(JSONObj *obj);
};
WRITE_CLASS_ENCODER(RGWDefaultRegionInfo);

struct RGWRegion {
  string name;
  string api_name;
  list<string> endpoints;
  bool is_master;

  string master_zone;
  map<string, RGWZone> zones;

  CephContext *cct;
  RGWRados *store;

  RGWRegion() : is_master(false) {}

  void encode(bufferlist& bl) const {
    ENCODE_START(1, 1, bl);
    ::encode(name, bl);
    ::encode(api_name, bl);
    ::encode(is_master, bl);
    ::encode(endpoints, bl);
    ::encode(master_zone, bl);
    ::encode(zones, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
    DECODE_START(1, bl);
    ::decode(name, bl);
    ::decode(api_name, bl);
    ::decode(is_master, bl);
    ::decode(endpoints, bl);
    ::decode(master_zone, bl);
    ::decode(zones, bl);
    DECODE_FINISH(bl);
  }

  int init(CephContext *_cct, RGWRados *_store, bool setup_region = true);
  int create_default();
  int store_info(bool exclusive);
  int read_info(const string& region_name);
  int read_default(RGWDefaultRegionInfo& default_region);
  int set_as_default();

  static string get_pool_name(CephContext *cct);

  void dump(Formatter *f) const;
  void decode_json(JSONObj *obj);
};
WRITE_CLASS_ENCODER(RGWRegion);

struct RGWRegionMap {
  Mutex lock;
  map<string, RGWRegion> regions;
  map<string, RGWRegion> regions_by_api;

  string master_region;

  RGWRegionMap() : lock("RGWRegionMap") {}

  void encode(bufferlist& bl) const;
  void decode(bufferlist::iterator& bl);

  void get_params(CephContext *cct, string& pool_name, string& oid);
  int read(CephContext *cct, RGWRados *store);
  int store(CephContext *cct, RGWRados *store);

  int update(RGWRegion& region);

  void dump(Formatter *f) const;
  void decode_json(JSONObj *obj);
};
WRITE_CLASS_ENCODER(RGWRegionMap);


  
class RGWRados
{
  friend class RGWGC;

  /** Open the pool used as root for this gateway */
  int open_root_pool_ctx();
  int open_gc_pool_ctx();

  int open_bucket_ctx(rgw_bucket& bucket, librados::IoCtx&  io_ctx);
  int open_bucket(rgw_bucket& bucket, librados::IoCtx&  io_ctx, string& bucket_oid);

  struct GetObjState {
    librados::IoCtx io_ctx;
    bool sent_data;

    GetObjState() : sent_data(false) {}
  };

  Mutex lock;
  SafeTimer *timer;

  class C_Tick : public Context {
    RGWRados *rados;
  public:
    C_Tick(RGWRados *_r) : rados(_r) {}
    void finish(int r) {
      rados->tick();
    }
  };

  RGWGC *gc;
  bool use_gc_thread;

  int num_watchers;
  RGWWatcher **watchers;
  uint64_t *watch_handles;
  librados::IoCtx root_pool_ctx;      // .rgw
  librados::IoCtx control_pool_ctx;   // .rgw.control

  Mutex bucket_id_lock;
  uint64_t max_bucket_id;

  int get_obj_state(RGWRadosCtx *rctx, rgw_obj& obj, RGWObjState **state);
  int append_atomic_test(RGWRadosCtx *rctx, rgw_obj& obj,
                         librados::ObjectOperation& op, RGWObjState **state);
  int prepare_atomic_for_write_impl(RGWRadosCtx *rctx, rgw_obj& obj,
                         librados::ObjectWriteOperation& op, RGWObjState **pstate,
			 bool reset_obj, const string *ptag);
  int prepare_atomic_for_write(RGWRadosCtx *rctx, rgw_obj& obj,
                         librados::ObjectWriteOperation& op, RGWObjState **pstate,
			 bool reset_obj, const string *ptag);

  void atomic_write_finish(RGWObjState *state, int r) {
    if (state && r == -ECANCELED) {
      state->clear();
    }
  }

  int clone_objs_impl(void *ctx, rgw_obj& dst_obj, 
                 vector<RGWCloneRangeInfo>& ranges,
                 map<string, bufferlist> attrs,
                 RGWObjCategory category,
                 time_t *pmtime,
                 bool truncate_dest,
                 bool exclusive,
                 pair<string, bufferlist> *cmp_xattr);

  virtual int clone_obj(void *ctx, rgw_obj& dst_obj, off_t dst_ofs,
                          rgw_obj& src_obj, off_t src_ofs,
                          uint64_t size, time_t *pmtime,
                          map<string, bufferlist> attrs,
                          RGWObjCategory category) {
    RGWCloneRangeInfo info;
    vector<RGWCloneRangeInfo> v;
    info.src = src_obj;
    info.src_ofs = src_ofs;
    info.dst_ofs = dst_ofs;
    info.len = size;
    v.push_back(info);
    return clone_objs(ctx, dst_obj, v, attrs, category, pmtime, true, false);
  }
  int delete_obj_impl(void *ctx, rgw_obj& src_obj);
  int complete_atomic_overwrite(RGWRadosCtx *rctx, RGWObjState *state, rgw_obj& obj);

  int update_placement_map();
  int select_bucket_placement(std::string& bucket_name, rgw_bucket& bucket);
  int store_bucket_info(RGWBucketInfo& info, map<string, bufferlist> *pattrs, bool exclusive);

protected:
  CephContext *cct;
  librados::Rados *rados;
  librados::IoCtx gc_pool_ctx;        // .rgw.gc

  bool pools_initialized;

  string region_name;
  string zone_name;

public:
  RGWRados() : lock("rados_timer_lock"), timer(NULL),
               gc(NULL), use_gc_thread(false),
               num_watchers(0), watchers(NULL), watch_handles(NULL),
               bucket_id_lock("rados_bucket_id"), max_bucket_id(0),
               cct(NULL), rados(NULL),
               pools_initialized(false) {}

  void set_context(CephContext *_cct) {
    cct = _cct;
  }

  void set_region(const string& name) {
    region_name = name;
  }

  void set_zone(const string& name) {
    zone_name = name;
  }

  RGWRegion region;
  RGWZoneParams zone;

  virtual ~RGWRados() {
    if (rados) {
      rados->shutdown();
      delete rados;
    }
  }

  int list_raw_prefixed_objs(string pool_name, const string& prefix, list<string>& result);
  int list_regions(list<string>& regions);
  int list_zones(list<string>& zones);

  void tick();

  CephContext *ctx() { return cct; }
  /** do all necessary setup of the storage device */
  int initialize(CephContext *_cct, bool _use_gc_thread) {
    set_context(_cct);
    use_gc_thread = _use_gc_thread;
    return initialize();
  }
  /** Initialize the RADOS instance and prepare to do other ops */
  virtual int init_rados();
  int init_complete();
  virtual int initialize();
  virtual void finalize();

  /** set up a bucket listing. handle is filled in. */
  virtual int list_buckets_init(RGWAccessHandle *handle);
  /** 
   * get the next bucket in the listing. obj is filled in,
   * handle is updated.
   */
  virtual int list_buckets_next(RGWObjEnt& obj, RGWAccessHandle *handle);

  /// list logs
  int log_list_init(const string& prefix, RGWAccessHandle *handle);
  int log_list_next(RGWAccessHandle handle, string *name);

  /// remove log
  int log_remove(const string& name);

  /// show log
  int log_show_init(const string& name, RGWAccessHandle *handle);
  int log_show_next(RGWAccessHandle handle, rgw_log_entry *entry);

  // log bandwidth info
  int log_usage(map<rgw_user_bucket, RGWUsageBatch>& usage_info);
  int read_usage(string& user, uint64_t start_epoch, uint64_t end_epoch, uint32_t max_entries,
                 bool *is_truncated, RGWUsageIter& read_iter, map<rgw_user_bucket, rgw_usage_log_entry>& usage);
  int trim_usage(string& user, uint64_t start_epoch, uint64_t end_epoch);

  /**
   * get listing of the objects in a bucket.
   * bucket: bucket to list contents of
   * max: maximum number of results to return
   * prefix: only return results that match this prefix
   * delim: do not include results that match this string.
   *     Any skipped results will have the matching portion of their name
   *     inserted in common_prefixes with a "true" mark.
   * marker: if filled in, begin the listing with this object.
   * result: the objects are put in here.
   * common_prefixes: if delim is filled in, any matching prefixes are placed
   *     here.
   */
  virtual int list_objects(rgw_bucket& bucket, int max, std::string& prefix, std::string& delim,
                   std::string& marker, std::vector<RGWObjEnt>& result, map<string, bool>& common_prefixes,
		   bool get_content_type, string& ns, bool *is_truncated, RGWAccessListFilter *filter);

  virtual int create_pool(rgw_bucket& bucket);

  /**
   * create a bucket with name bucket and the given list of attrs
   * returns 0 on success, -ERR# otherwise.
   */
  virtual int create_bucket(string& owner, rgw_bucket& bucket,
                            map<std::string,bufferlist>& attrs,
                            bool exclusive = true);
  virtual int add_bucket_placement(std::string& new_pool);
  virtual int remove_bucket_placement(std::string& new_pool);
  virtual int list_placement_set(set<string>& names);
  virtual int create_pools(vector<string>& names, vector<int>& retcodes);

  struct PutObjMetaExtraParams {
    time_t *mtime;
    map<std::string, bufferlist>* rmattrs;
    const bufferlist *data;
    RGWObjManifest *manifest;
    const string *ptag;
    list<string> *remove_objs;
    bool modify_version;
    obj_version *objv;

    PutObjMetaExtraParams() : mtime(NULL), rmattrs(NULL),
                     data(NULL), manifest(NULL), ptag(NULL),
                     remove_objs(NULL), modify_version(false), objv(NULL) {}
  };

  /** Write/overwrite an object to the bucket storage. */
  virtual int put_obj_meta_impl(void *ctx, rgw_obj& obj, uint64_t size, time_t *mtime,
              map<std::string, bufferlist>& attrs, RGWObjCategory category, int flags,
              map<std::string, bufferlist>* rmattrs, const bufferlist *data,
              RGWObjManifest *manifest, const string *ptag, list<string> *remove_objs,
              bool modify_version, obj_version *objv);

  virtual int put_obj_meta(void *ctx, rgw_obj& obj, uint64_t size,
              map<std::string, bufferlist>& attrs, RGWObjCategory category, int flags,
              const bufferlist *data = NULL) {
    return put_obj_meta_impl(ctx, obj, size, NULL, attrs, category, flags,
                        NULL, data, NULL, NULL, NULL,
                        false, NULL);
  }

  virtual int put_obj_meta(void *ctx, rgw_obj& obj, uint64_t size,  map<std::string, bufferlist>& attrs,
                           RGWObjCategory category, int flags, PutObjMetaExtraParams& params) {
    return put_obj_meta_impl(ctx, obj, size, params.mtime, attrs, category, flags,
                        params.rmattrs, params.data, params.manifest, params.ptag, params.remove_objs,
                        params.modify_version, params.objv);
  }

  virtual int put_obj_data(void *ctx, rgw_obj& obj, const char *data,
              off_t ofs, size_t len, bool exclusive);
  virtual int aio_put_obj_data(void *ctx, rgw_obj& obj, bufferlist& bl,
                               off_t ofs, bool exclusive, void **handle);
  /* note that put_obj doesn't set category on an object, only use it for none user objects */
  int put_system_obj(void *ctx, rgw_obj& obj, const char *data, size_t len, bool exclusive,
              time_t *mtime, map<std::string, bufferlist>& attrs, obj_version *objv) {
    bufferlist bl;
    bl.append(data, len);
    int flags = PUT_OBJ_CREATE;
    if (exclusive)
      flags |= PUT_OBJ_EXCL;

    PutObjMetaExtraParams ep;
    ep.mtime = mtime;
    ep.data = &bl;
    ep.modify_version = true;
    ep.objv = objv;

    int ret = put_obj_meta(ctx, obj, len, attrs, RGW_OBJ_CATEGORY_NONE, flags, ep);
    return ret;
  }
  virtual int aio_wait(void *handle);
  virtual bool aio_completed(void *handle);
  virtual int clone_objs(void *ctx, rgw_obj& dst_obj, 
                         vector<RGWCloneRangeInfo>& ranges,
                         map<string, bufferlist> attrs,
                         RGWObjCategory category,
                         time_t *pmtime, bool truncate_dest, bool exclusive) {
    return clone_objs(ctx, dst_obj, ranges, attrs, category, pmtime, truncate_dest, exclusive, NULL);
  }

  int clone_objs(void *ctx, rgw_obj& dst_obj, 
                 vector<RGWCloneRangeInfo>& ranges,
                 map<string, bufferlist> attrs,
                 RGWObjCategory category,
                 time_t *pmtime,
                 bool truncate_dest,
                 bool exclusive,
                 pair<string, bufferlist> *cmp_xattr);

  int clone_obj_cond(void *ctx, rgw_obj& dst_obj, off_t dst_ofs,
                rgw_obj& src_obj, off_t src_ofs,
                uint64_t size, map<string, bufferlist> attrs,
                RGWObjCategory category,
                time_t *pmtime,
                bool truncate_dest,
                bool exclusive,
                pair<string, bufferlist> *xattr_cond) {
    RGWCloneRangeInfo info;
    vector<RGWCloneRangeInfo> v;
    info.src = src_obj;
    info.src_ofs = src_ofs;
    info.dst_ofs = dst_ofs;
    info.len = size;
    v.push_back(info);
    return clone_objs(ctx, dst_obj, v, attrs, category, pmtime, truncate_dest, exclusive, xattr_cond);
  }

  /**
   * Copy an object.
   * dest_obj: the object to copy into
   * src_obj: the object to copy from
   * attrs: if replace_attrs is set then these are placed on the new object
   * err: stores any errors resulting from the get of the original object
   * Returns: 0 on success, -ERR# otherwise.
   */
  virtual int copy_obj(void *ctx, rgw_obj& dest_obj,
               rgw_obj& src_obj,
               time_t *mtime,
               const time_t *mod_ptr,
               const time_t *unmod_ptr,
               const char *if_match,
               const char *if_nomatch,
               bool replace_attrs,
               map<std::string, bufferlist>& attrs,
               RGWObjCategory category,
               struct rgw_err *err);

  int copy_obj_data(void *ctx,
	       void *handle, off_t end,
               rgw_obj& dest_obj,
               rgw_obj& src_obj,
	       time_t *mtime,
               map<string, bufferlist>& attrs,
               RGWObjCategory category,
               struct rgw_err *err);
  /**
   * Delete a bucket.
   * bucket: the name of the bucket to delete
   * Returns 0 on success, -ERR# otherwise.
   */  virtual int delete_bucket(rgw_bucket& bucket);

  int set_bucket_owner(rgw_bucket& bucket, ACLOwner& owner);
  int set_buckets_enabled(std::vector<rgw_bucket>& buckets, bool enabled);
  int bucket_suspended(rgw_bucket& bucket, bool *suspended);

  /** Delete an object.*/
  virtual int delete_obj(void *ctx, rgw_obj& src_obj);

  /** Remove an object from the bucket index */
  int delete_obj_index(rgw_obj& obj);

  /**
   * Get the attributes for an object.
   * bucket: name of the bucket holding the object.
   * obj: name of the object
   * name: name of the attr to retrieve
   * dest: bufferlist to store the result in
   * Returns: 0 on success, -ERR# otherwise.
   */
  virtual int get_attr(void *ctx, rgw_obj& obj, const char *name, bufferlist& dest);

  /**
   * Set an attr on an object.
   * bucket: name of the bucket holding the object
   * obj: name of the object to set the attr on
   * name: the attr to set
   * bl: the contents of the attr
   * Returns: 0 on success, -ERR# otherwise.
   */
  virtual int set_attr(void *ctx, rgw_obj& obj, const char *name, bufferlist& bl);

  virtual int set_attrs(void *ctx, rgw_obj& obj,
                        map<string, bufferlist>& attrs,
                        map<string, bufferlist>* rmattrs);

/**
 * Get data about an object out of RADOS and into memory.
 * bucket: name of the bucket the object is in.
 * obj: name/key of the object to read
 * data: if get_data==true, this pointer will be set
 *    to an address containing the object's data/value
 * ofs: the offset of the object to read from
 * end: the point in the object to stop reading
 * attrs: if non-NULL, the pointed-to map will contain
 *    all the attrs of the object when this function returns
 * mod_ptr: if non-NULL, compares the object's mtime to *mod_ptr,
 *    and if mtime is smaller it fails.
 * unmod_ptr: if non-NULL, compares the object's mtime to *unmod_ptr,
 *    and if mtime is >= it fails.
 * if_match/nomatch: if non-NULL, compares the object's etag attr
 *    to the string and, if it doesn't/does match, fails out.
 * err: Many errors will result in this structure being filled
 *    with extra informatin on the error.
 * Returns: -ERR# on failure, otherwise
 *          (if get_data==true) length of read data,
 *          (if get_data==false) length of the object
 */
  virtual int prepare_get_obj(void *ctx, rgw_obj& obj,
            off_t *ofs, off_t *end,
            map<string, bufferlist> *attrs,
            const time_t *mod_ptr,
            const time_t *unmod_ptr,
            time_t *lastmod,
            const char *if_match,
            const char *if_nomatch,
            uint64_t *total_size,
            uint64_t *obj_size,
            void **handle,
            struct rgw_err *err);

  virtual int get_obj(void *ctx, void **handle, rgw_obj& obj,
                      bufferlist& bl, off_t ofs, off_t end);

  virtual void finish_get_obj(void **handle);

  int iterate_obj(void *ctx, rgw_obj& obj,
                  off_t ofs, off_t end,
                  uint64_t max_chunk_size,
                  int (*iterate_obj_cb)(rgw_obj&, off_t, off_t, off_t, bool, RGWObjState *, void *),
                  void *arg);

  int get_obj_iterate(void *ctx, void **handle, rgw_obj& obj,
                      off_t ofs, off_t end,
	              RGWGetDataCB *cb);

  int get_obj_iterate_cb(void *ctx, RGWObjState *astate,
                         rgw_obj& obj,
                         off_t obj_ofs, off_t read_ofs, off_t len,
                         bool is_head_obj, void *arg);

  void get_obj_aio_completion_cb(librados::completion_t cb, void *arg);

  /**
   * a simple object read without keeping state
   */
  virtual int read(void *ctx, rgw_obj& obj, off_t ofs, size_t size, bufferlist& bl);

  virtual int obj_stat(void *ctx, rgw_obj& obj, uint64_t *psize, time_t *pmtime,
                       uint64_t *epoch, map<string, bufferlist> *attrs, bufferlist *first_chunk,
                       obj_version *objv);

  virtual bool supports_omap() { return true; }
  virtual int omap_get_all(rgw_obj& obj, bufferlist& header, std::map<string, bufferlist>& m);
  virtual int omap_set(rgw_obj& obj, std::string& key, bufferlist& bl);
  virtual int omap_set(rgw_obj& obj, map<std::string, bufferlist>& m);
  virtual int omap_del(rgw_obj& obj, std::string& key);
  virtual int update_containers_stats(map<string, RGWBucketEnt>& m);
  virtual int append_async(rgw_obj& obj, size_t size, bufferlist& bl);

  virtual int init_watch();
  virtual void finalize_watch();
  virtual int distribute(const string& key, bufferlist& bl);
  virtual int watch_cb(int opcode, uint64_t ver, bufferlist& bl) { return 0; }
  void pick_control_oid(const string& key, string& notify_oid);

  void *create_context(void *user_ctx) {
    RGWRadosCtx *rctx = new RGWRadosCtx(this);
    rctx->user_ctx = user_ctx;
    return rctx;
  }
  void destroy_context(void *ctx) {
    delete static_cast<RGWRadosCtx *>(ctx);
  }
  void set_atomic(void *ctx, rgw_obj& obj) {
    RGWRadosCtx *rctx = static_cast<RGWRadosCtx *>(ctx);
    rctx->set_atomic(obj);
  }
  void set_prefetch_data(void *ctx, rgw_obj& obj) {
    RGWRadosCtx *rctx = static_cast<RGWRadosCtx *>(ctx);
    rctx->set_prefetch_data(obj);
  }
  // to notify upper layer that we need to do some operation on an object, and it's up to
  // the upper layer to schedule this operation.. e.g., log intent in intent log
  void set_intent_cb(void *ctx, int (*cb)(RGWRados *store, void *user_ctx, rgw_obj& obj, RGWIntentEvent intent)) {
    RGWRadosCtx *rctx = static_cast<RGWRadosCtx *>(ctx);
    rctx->set_intent_cb(cb);
  }

  int decode_policy(bufferlist& bl, ACLOwner *owner);
  int get_bucket_stats(rgw_bucket& bucket, map<RGWObjCategory, RGWBucketStats>& stats);
  virtual int get_bucket_info(void *ctx, string& bucket_name, RGWBucketInfo& info, map<string, bufferlist> *pattrs = NULL);
  virtual int put_bucket_info(string& bucket_name, RGWBucketInfo& info, bool exclusive, map<string, bufferlist> *pattrs);

  int cls_rgw_init_index(librados::IoCtx& io_ctx, librados::ObjectWriteOperation& op, string& oid);
  int cls_obj_prepare_op(rgw_bucket& bucket, uint8_t op, string& tag,
                         string& name, string& locator);
  int cls_obj_complete_op(rgw_bucket& bucket, uint8_t op, string& tag, uint64_t epoch,
                          RGWObjEnt& ent, RGWObjCategory category, list<string> *remove_objs);
  int cls_obj_complete_add(rgw_bucket& bucket, string& tag, uint64_t epoch, RGWObjEnt& ent, RGWObjCategory category, list<string> *remove_objs);
  int cls_obj_complete_del(rgw_bucket& bucket, string& tag, uint64_t epoch, string& name);
  int cls_obj_complete_cancel(rgw_bucket& bucket, string& tag, string& name);
  int cls_obj_set_bucket_tag_timeout(rgw_bucket& bucket, uint64_t timeout);
  int cls_bucket_list(rgw_bucket& bucket, string start, string prefix, uint32_t num,
                      map<string, RGWObjEnt>& m, bool *is_truncated,
                      string *last_entry, bool (*force_check_filter)(const string&  name) = NULL);
  int cls_bucket_head(rgw_bucket& bucket, struct rgw_bucket_dir_header& header);
  int prepare_update_index(RGWObjState *state, rgw_bucket& bucket,
                           rgw_obj& oid, string& tag);
  int complete_update_index(rgw_bucket& bucket, string& oid, string& tag, uint64_t epoch, uint64_t size,
                            utime_t& ut, string& etag, string& content_type, bufferlist *acl_bl, RGWObjCategory category,
			    list<string> *remove_objs);
  int complete_update_index_del(rgw_bucket& bucket, string& oid, string& tag, uint64_t epoch) {
    if (bucket_is_system(bucket))
      return 0;

    return cls_obj_complete_del(bucket, tag, epoch, oid);
  }
  int complete_update_index_cancel(rgw_bucket& bucket, string& oid, string& tag) {
    if (bucket_is_system(bucket))
      return 0;

    return cls_obj_complete_cancel(bucket, tag, oid);
  }

  int cls_obj_usage_log_add(const string& oid, rgw_usage_log_info& info);
  int cls_obj_usage_log_read(string& oid, string& user, uint64_t start_epoch, uint64_t end_epoch, uint32_t max_entries,
                             string& read_iter, map<rgw_user_bucket, rgw_usage_log_entry>& usage, bool *is_truncated);
  int cls_obj_usage_log_trim(string& oid, string& user, uint64_t start_epoch, uint64_t end_epoch);

  /// clean up/process any temporary objects older than given date[/time]
  int remove_temp_objects(string date, string time);

  int gc_operate(string& oid, librados::ObjectWriteOperation *op);
  int gc_aio_operate(string& oid, librados::ObjectWriteOperation *op);
  int gc_operate(string& oid, librados::ObjectReadOperation *op, bufferlist *pbl);

  int list_gc_objs(int *index, string& marker, uint32_t max, std::list<cls_rgw_gc_obj_info>& result, bool *truncated);
  int process_gc();
  int defer_gc(void *ctx, rgw_obj& obj);

  int bucket_check_index(rgw_bucket& bucket,
                         map<RGWObjCategory, RGWBucketStats> *existing_stats,
                         map<RGWObjCategory, RGWBucketStats> *calculated_stats);
  int bucket_rebuild_index(rgw_bucket& bucket);
  int remove_objs_from_index(rgw_bucket& bucket, list<string>& oid_list);

 private:
  int process_intent_log(rgw_bucket& bucket, string& oid,
			 time_t epoch, int flags, bool purge);
  /**
   * Check the actual on-disk state of the object specified
   * by list_state, and fill in the time and size of object.
   * Then append any changes to suggested_updates for
   * the rgw class' dir_suggest_changes function.
   *
   * Note that this can maul list_state; don't use it afterwards. Also
   * it expects object to already be filled in from list_state; it only
   * sets the size and mtime.
   *
   * Returns 0 on success, -ENOENT if the object doesn't exist on disk,
   * and -errno on other failures. (-ENOENT is not a failure, and it
   * will encode that info as a suggested update.)
   */
  int check_disk_state(librados::IoCtx io_ctx,
                       rgw_bucket& bucket,
                       rgw_bucket_dir_entry& list_state,
                       RGWObjEnt& object,
                       bufferlist& suggested_updates);

  bool bucket_is_system(rgw_bucket& bucket) {
    return (bucket.name[0] == '.');
  }

  /**
   * Init pool iteration
   * bucket: pool name in a bucket object
   * ctx: context object to use for the iteration
   * Returns: 0 on success, -ERR# otherwise.
   */
  int pool_iterate_begin(rgw_bucket& bucket, RGWPoolIterCtx& ctx);
  /**
   * Iterate over pool return object names, use optional filter
   * ctx: iteration context, initialized with pool_iterate_begin()
   * num: max number of objects to return
   * objs: a vector that the results will append into
   * is_truncated: if not NULL, will hold true iff iteration is complete
   * filter: if not NULL, will be used to filter returned objects
   * Returns: 0 on success, -ERR# otherwise.
   */
  int pool_iterate(RGWPoolIterCtx& ctx, uint32_t num, vector<RGWObjEnt>& objs,
                   bool *is_truncated, RGWAccessListFilter *filter);

  int list_raw_objects(rgw_bucket& pool, const string& prefix_filter, int max,
                       RGWListRawObjsCtx& ctx, vector<string>& oids,
                       bool *is_truncated);

  uint64_t instance_id();
  uint64_t next_bucket_id();

};

class RGWStoreManager {
public:
  RGWStoreManager() {}
  static RGWRados *get_storage(CephContext *cct, bool use_gc_thread) {
    RGWRados *store = init_storage_provider(cct, use_gc_thread);
    return store;
  }
  static RGWRados *get_raw_storage(CephContext *cct) {
    RGWRados *store = init_raw_storage_provider(cct);
    return store;
  }
  static RGWRados *init_storage_provider(CephContext *cct, bool use_gc_thread);
  static RGWRados *init_raw_storage_provider(CephContext *cct);
  static void close_storage(RGWRados *store);

};



#endif
