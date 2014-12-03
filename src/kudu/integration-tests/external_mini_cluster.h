// Copyright (c) 2013, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.
#ifndef KUDU_INTEGRATION_TESTS_EXTERNAL_MINI_CLUSTER_H
#define KUDU_INTEGRATION_TESTS_EXTERNAL_MINI_CLUSTER_H

#include <string>
#include <tr1/memory>
#include <vector>

#include "kudu/client/client.h"
#include "kudu/gutil/gscoped_ptr.h"
#include "kudu/gutil/macros.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/util/monotime.h"
#include "kudu/util/net/net_util.h"
#include "kudu/util/status.h"

namespace kudu {

class ExternalDaemon;
class ExternalMaster;
class ExternalTabletServer;
class HostPort;
class NodeInstancePB;
class Sockaddr;
class Subprocess;

namespace master {
class MasterServiceProxy;
} // namespace master

namespace rpc {
class Messenger;
} // namespace rpc

namespace server {
class ServerStatusPB;
} // namespace server

struct ExternalMiniClusterOptions {
  ExternalMiniClusterOptions();
  ~ExternalMiniClusterOptions();

  // Number of masters to start.
  // Default: 1
  int num_masters;

  // Number of TS to start.
  // Default: 1
  int num_tablet_servers;

  // Directory in which to store data.
  // Default: "", which auto-generates a unique path for this cluster.
  std::string data_root;

  // The path where the kudu daemons should be run from.
  // Default: "", which uses the same path as the currently running executable.
  // This works for unit tests, since they all end up in build/latest/.
  std::string daemon_bin_path;

  // Extra flags for tablet servers and masters respectively.
  //
  // In these flags, you may use the special string '${index}' which will
  // be substituted with the index of the tablet server or master.
  std::vector<std::string> extra_tserver_flags;
  std::vector<std::string> extra_master_flags;

  // If more than one master is specified, list of ports for the
  // masters in a quorum. Port at index 0 is used for the leader
  // master.
  std::vector<uint16_t> master_rpc_ports;
};

// A mini-cluster made up of subprocesses running each of the daemons
// separately. This is useful for black-box or grey-box failure testing
// purposes -- it provides the ability to forcibly kill or stop particular
// cluster participants, which isn't feasible in the normal MiniCluster.
// On the other hand, there is little access to inspect the internal state
// of the daemons.
class ExternalMiniCluster {
 public:
  explicit ExternalMiniCluster(const ExternalMiniClusterOptions& opts);
  ~ExternalMiniCluster();

  // Start the cluster.
  Status Start();

  // Like the previous method but performs initialization synchronously, i.e.
  // this will wait for all TS's to be started and initialized. Tests should
  // use this if they interact with tablets immediately after Start();
  Status StartSync();

  // Add a new TS to the cluster. The new TS is started.
  // Requires that the master is already running.
  Status AddTabletServer();

  // Shuts down the cluster.
  // Currently, this uses SIGKILL on each daemon for a non-graceful shutdown.
  void Shutdown();

  // Return a pointer to the running leader master. This may be NULL
  // if the cluster is not started.
  //
  // TODO: Use the appropriate RPC here to return the leader master,
  // to allow some of the existing tests (e.g., raft_consensus-itest)
  // to use multiple masters.
  ExternalMaster* leader_master() { return master(0); }

  // If this cluster is configured for a single non-distributed
  // master, return the single master or NULL if the master is not
  // started. Exits with a CHECK failure if there are multiple
  // masters.
  ExternalMaster* master() {
    CHECK_EQ(masters_.size(), 1)
        << "master() should not be used with multiple masters, use leader_master() instead.";
    return master(0);
  }

  // Return master at 'idx' or NULL if the master at 'idx' has not
  // been started.
  ExternalMaster* master(int idx) {
    CHECK_LT(idx, masters_.size());
    return masters_[idx].get();
  }

  ExternalTabletServer* tablet_server(int idx) {
    CHECK_LT(idx, tablet_servers_.size());
    return tablet_servers_[idx].get();
  }

  int num_tablet_servers() const {
    return tablet_servers_.size();
  }

  int num_masters() const {
    return masters_.size();
  }

  // If the cluster is configured for a single non-distributed master,
  // return a proxy to that master. Requires that the single master is
  // running.
  std::tr1::shared_ptr<master::MasterServiceProxy> master_proxy();

  // Returns an RPC proxy to the master at 'idx'. Requires that the
  // master at 'idx' is running.
  std::tr1::shared_ptr<master::MasterServiceProxy> master_proxy(int idx);

  // Wait until the number of registered tablet servers reaches the
  // given count on at least one of the running masters.  Returns
  // Status::TimedOut if the desired count is not achieved with the
  // given timeout.
  Status WaitForTabletServerCount(int count, const MonoDelta& timeout);

  // Create a client configured to talk to this cluster.
  // Builder may contain override options for the client. The master address will
  // be overridden to talk to the running master.
  //
  // REQUIRES: the cluster must have already been Start()ed.
  Status CreateClient(client::KuduClientBuilder& builder,
                      std::tr1::shared_ptr<client::KuduClient>* client);

 private:
  FRIEND_TEST(MasterFailoverTest, TestKillAnyMaster);

  Status StartSingleMaster();

  Status StartDistributedMasters();

  std::string GetBinaryPath(const std::string& binary) const;
  std::string GetDataPath(const std::string& daemon_id) const;

  Status DeduceBinRoot(std::string* ret);
  Status HandleOptions();

  const ExternalMiniClusterOptions opts_;

  // The root for binaries.
  std::string daemon_bin_path_;

  std::string data_root_;

  bool started_;

  std::vector<scoped_refptr<ExternalMaster> > masters_;
  std::vector<scoped_refptr<ExternalTabletServer> > tablet_servers_;

  std::tr1::shared_ptr<rpc::Messenger> messenger_;

  DISALLOW_COPY_AND_ASSIGN(ExternalMiniCluster);
};

class ExternalDaemon : public RefCountedThreadSafe<ExternalDaemon> {
 public:
  ExternalDaemon(const std::string& exe, const std::string& data_dir,
                 const std::vector<std::string>& extra_flags);

  HostPort bound_rpc_hostport() const;
  Sockaddr bound_rpc_addr() const;
  HostPort bound_http_hostport() const;
  const NodeInstancePB& instance_id() const;

  // Sends a SIGSTOP signal to the daemon.
  Status Pause();

  // Sends a SIGCONT signal to the daemon.
  Status Resume();

  virtual void Shutdown();

 protected:
  friend class RefCountedThreadSafe<ExternalDaemon>;
  virtual ~ExternalDaemon();
  Status StartProcess(const std::vector<std::string>& flags);

  const std::string exe_;
  const std::string data_dir_;
  const std::vector<std::string> extra_flags_;

  gscoped_ptr<Subprocess> process_;

  gscoped_ptr<server::ServerStatusPB> status_;

  // These capture the daemons parameters and running ports and
  // are used to Restart() the daemon with the same parameters.
  HostPort bound_rpc_;
  HostPort bound_http_;

  DISALLOW_COPY_AND_ASSIGN(ExternalDaemon);
};


class ExternalMaster : public ExternalDaemon {
 public:
  ExternalMaster(const std::string& exe, const std::string& data_dir,
                 const std::vector<std::string>& extra_flags);

  ExternalMaster(const std::string& exe, const std::string& data_dir,
                 const std::string& rpc_bind_address,
                 const std::vector<std::string>& extra_flags);

  Status Start();

  // Restarts the daemon.
  // Requires that it has previously been shutdown.
  Status Restart() WARN_UNUSED_RESULT;


 private:
  friend class RefCountedThreadSafe<ExternalMaster>;
  virtual ~ExternalMaster();

  const std::string rpc_bind_address_;
};

class ExternalTabletServer : public ExternalDaemon {
 public:
  ExternalTabletServer(const std::string& exe, const std::string& data_dir,
                       const std::vector<HostPort>& master_addrs,
                       const std::vector<std::string>& extra_flags);

  Status Start();

  // Restarts the daemon.
  // Requires that it has previously been shutdown.
  Status Restart() WARN_UNUSED_RESULT;


 private:
  const std::string master_addrs_;

  friend class RefCountedThreadSafe<ExternalTabletServer>;
  virtual ~ExternalTabletServer();
};

} // namespace kudu
#endif /* KUDU_INTEGRATION_TESTS_EXTERNAL_MINI_CLUSTER_H */
