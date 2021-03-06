/*!
 * Copyright (c) 2015 by Contributors
 * \file kvstore.h
 * \brief key-value store interface for mxnet
 */
#ifndef MXNET_KVSTORE_H_
#define MXNET_KVSTORE_H_
#include <dmlc/io.h>
#include <vector>
#include <string>
#include <functional>
#include "./ndarray.h"

namespace mxnet {
/*!
 * \brief distributed key-value store
 *
 * A distributed key-value store for data synchronization over multiple
 * devices/machines. It support user-defined updater.
 */
class KVStore {
 public:
  /*! \brief virtual destructor */
  virtual ~KVStore() {}

  /*!
   * \brief Factory function to create a new KVStore.
   * \param type The type of the kvstore,
   *   'local' : multi-devices on a single machine. can be also
   *      'local_update_cpu', 'local_allreduce_cpu'
   *   'device' or 'local_allreduce_device' : same to local but use gpus for kv
   *       allreduce
   *   'dist_sync' : multi-machines with BSP
   *   'dist_async' : multi-machines with partical asynchronous
   * \return a new created KVStore.
   */
  static KVStore *Create(const char *type = "local");

  /**
   * \brief return the type
   */
  inline const std::string& type() { return type_; }

  /*!
   * \brief Initialize a list of key-value pair to the store.
   *
   * One should initalize the key before \ref Push and \ref Pull, and a key
   * should be only initialized once
   *
   * It returns after data have been initialized successfully
   *
   * \param keys a list of unique keys
   * \param values a list of values
   */
  virtual void Init(const std::vector<int>& keys,
                    const std::vector<NDArray>& values) = 0;
  /*!
   * \brief push a list of key-value pairs into the store
   *
   * If a key appears mulitple times in \a keys, then the according values will
   * be aggregated (summed) before pushing.
   *
   * The (aggregated) values are merged into the store one by one
   *
   * \code
   * updater(key, value, &value_in_store);
   * \endcode
   *
   * One can set a user-defined updater by \ref set_updater. The default updater
   * is Assign.
   *
   * This function returns after adding a push operator to the engine. Any
   * following operator requiring writing value will be blocked until the
   * actual push is finished. One can wait the push is finished by
   *
   * - when type == "local"
   * \code
   * for (auto& v : values) v.WaitToWrite()
   * \endcode
   *
   * - when type == "dist"
   * \code
   * Wait(keys);
   * \endcode
   *
   * One must call Init() on every key before. And the value NDArray should be
   * always has the same shape as being inited.
   *
   * \param keys the list of keys
   * \param values the list of values
   * \param priority Priority of the action.
   */
  virtual void Push(const std::vector<int>& keys,
                    const std::vector<NDArray>& values,
                    int priority = 0)  = 0;
  /*!
   * \brief pull a list of key-value pairs from the store
   *
   * One must call Init() on \a key before. And \a value should be pre-allocated
   *
   * This function returns after adding a pull operator to the engine. Any
   * following operator requiring reading value will be blocked until the
   * actual pull is finished. One can wait the pull is finished by
   *
   * - when type == "local"
   * \code
   * for (auto& v : values) v.WaitToRead()
   * \endcode
   *
   * - when type == "dist"
   * \code
   * Wait(keys);
   * \endcode
   *
   * \param keys the list of keys
   * \param values the list of buffers for the pulled data, they should be preallocated
   * \param priority Priority of the action.
   */
  virtual void Pull(const std::vector<int>& keys,
                    const std::vector<NDArray*>& values,
                    int priority = 0) = 0;

  /**
   * \brief the prototype of user-defined updater
   */
  typedef std::function<void(int, const NDArray&, NDArray*)> Updater;

  /*!
   * \brief set an updater
   *
   * Given a key, assume \a x is the received (pushed) value and \a y is the
   * value stored on the store node. The store updates \a y by `h(x, &y)`. The
   * default \a h is ASSIGN, namely `*y = x`.
   *
   * \param updater user-defined updater, default is assign
   */
  void set_updater(Updater updater) {
    updater_ = updater;
  }

  /******************************************************
   * the following are used for multi-machines.
   ******************************************************/

  /**
   * \return whether or not is in distributed computing
   */
  virtual bool IsDistributed() const {
    return false;
  }

  /**
   * \return whether or not this process is a worker node.
   *
   * Always returns true when type == "local"
   */
  static bool IsWorkerNode() {
    char* role_str = getenv("DMLC_ROLE");
    return (role_str == nullptr) || (!strcmp(role_str, "worker"));
  }

  /**
   * \return whether or not this process is a server node.
   *
   * Always returns false when type == "local"
   */
  static bool IsServerNode() {
    char* role_str = getenv("DMLC_ROLE");
    return (role_str != nullptr) && (!strcmp(role_str, "server"));
  }


  /**
   * \return whether or not this process is a scheduler node.
   *
   * Always returns false when type == "local"
   */
  static bool IsSchedulerNode() {
    char* role_str = getenv("DMLC_ROLE");
    return (role_str != nullptr) && (!strcmp(role_str, "scheduler"));
  }

  /*!
   * \return The rank of this node in its group, which is in [0,
   * GroupSize).
   *
   * Always return 0 when type == "local"
   */
  virtual int get_rank() const {
    return 0;
  }

  /*!
   * \return The number of nodes in this group.
   *
   * Always returns 1 when type == "local". Otherwise, returns
   *
   * - number of workers if if `IsWorkerNode() == true`,
   * - number of servers if if `IsServerNode() == true`,
   * - 1 if `IsSchedulerNode() == true`,
   */
  virtual int get_group_size() const {
    return 1;
  }

  /**
   * \brief Wait until all pushes and pulls issued on each key have been
   * finished
   *
   * \param keys a list of keys
   */
  virtual void Wait(const std::vector<int>& keys) { }

  /**
   * \brief Wait until all pushes and pulls issued before have been finished
   */
  virtual void WaitAll() { }

  /*!
   * \brief global barrier among all worker machines
   *
   * For example, assume there are n machines, we want to let machine 0 first
   * init the values, and then pull the inited value to all machines. Before
   * pulling, we can place a barrier to guarantee that the initialization is
   * finished.
   *
   * \code
   * // this codes run on n machines in parallel
   * if (get_rank() == 0) {
   *   Init(keys, values);
   * }
   * Barrier();
   * Pull(keys, values);
   * \endcode
   *
   * But note that, this functions only blocks the main thread of workers until
   * all of them are reached this point. It doesn't guarantee that all
   * operations issued before are actually finished, such as \ref Push and \ref
   * Pull. In that case, we need to call \ref Wait or \ref WaitAll
   */
  virtual void Barrier() { }

  /**
   * \brief Send a command to all server nodes
   *
   * Send a command to all server nodes, which will make each server node run
   * \a controller
   *
   * This function returns after the command has been executed in all server nodes
   *
   * \param cmd_id the head of the command
   * \param cmd_body the body of the command
   */
  virtual void SendCommandToServers(int cmd_id, const std::string& cmd_body) { }

  /**
   * \brief the prototype of a server controller
   */
  typedef std::function<void(int, const std::string&)> Controller;

  /**
   * \brief Run as server (or scheduler)
   *
   * The behavior of a server:
   * \code
   * while(receive(x)) {
   *   if (IsCommand(x)) controller(x)
   *   else if (IsKeyValue(x)) updater(x)
   * }
   * \endcode
   *
   * \param controller the user-defined server controller
   */
  virtual void RunServer(const Controller& controller) { }

 protected:
  /**
   * \brief the user-defined  updater
   */
  Updater updater_;

  /**
   * \brief the kvstore type
   */
  std::string type_;
};

}  // namespace mxnet
#endif  // MXNET_KVSTORE_H_
