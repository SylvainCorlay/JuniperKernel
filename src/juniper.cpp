#include <string>
#include <thread>
#include <fstream>
#include <Rcpp.h>
#include <zmq.hpp>
#include <zmq_addon.hpp>
#include <json.hpp>
#include <juniper/juniper.h>
#include <juniper/sockets.h>
#include <juniper/background.h>
#include <unistd.h>



#include <stdio.h>
#include <execinfo.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
void handler(int sig) {
  void *array[10];
  size_t size;

  // get void*'s for all entries on the stack
  size = backtrace(array, 10);

  // print out all the frames to stderr
  fprintf(stderr, "Error: signal %d:\n", sig);
  backtrace_symbols_fd(array, size, STDERR_FILENO);
  exit(1);
}

class JuniperKernel {
  public:
    JuniperKernel(const config& conf):
      _ctx(new zmq::context_t(1)),

      // these are the 3 incoming Jupyter channels
      _stdin(new zmq::socket_t(*_ctx, zmq::socket_type::router)),

      // these are internal routing sockets that push messages (e.g.
      // poison pills, results, etc.) to the heartbeat thread and
      // iopub thread.
      _inproc_pub(new zmq::socket_t(*_ctx, zmq::socket_type::pub)),
      _inproc_sig(new zmq::socket_t(*_ctx, zmq::socket_type::pub)),

      _hbport(conf.hb_port),
      _ioport(conf.iopub_port),
      _shellport(conf.shell_port),
      _cntrlport(conf.control_port),
      _key(conf.key),
      _sig(conf.signature_scheme) {
        char sep = (conf.transport=="tcp") ? ':' : '-';
        _endpoint = conf.transport + "://" + conf.ip + sep;

        // socket setup
        init_socket(_stdin, _endpoint + conf.stdin_port);

        // iopub and hbeat get their own threads and we communicate
        // via the inproc topics sig/sub
        //
        // these get bound and cleaned by THIS thread
        init_socket(_inproc_pub, INPROC_PUB);
        init_socket(_inproc_sig, INPROC_SIG);
    }

    static JuniperKernel* make(const std::string& connection_file) {
      std::ifstream ifs(connection_file);
      nlohmann::json connection_info = nlohmann::json::parse(ifs);
      config conf = {
        std::to_string(connection_info["control_port"    ].get<int        >()),
        std::to_string(connection_info["hb_port"         ].get<int        >()),
        std::to_string(connection_info["iopub_port"      ].get<int        >()),
                       connection_info["ip"              ].get<std::string>(),
                       connection_info["key"             ].get<std::string>(),
        std::to_string(connection_info["shell_port"      ].get<int        >()),
                       connection_info["signature_scheme"].get<std::string>(),
        std::to_string(connection_info["stdin_port"      ].get<int        >()),
                       connection_info["transport"       ].get<std::string>(),
      };
      conf.print_conf();
      return new JuniperKernel(conf);
    }

    // start the background threads
    // called as part of the kernel boot sequence
    void start_bg_threads() {
      _hbthread = start_hb_thread(*_ctx, _endpoint + _hbport);
      _iothread = start_io_thread(*_ctx, _endpoint + _ioport);
    }

    // runs in the main the thread, polls shell and controller
     void run() const {

       zmq::socket_t* cntrl = new zmq::socket_t(*_ctx, zmq::socket_type::router);
       zmq::socket_t* shell = new zmq::socket_t(*_ctx, zmq::socket_type::router);

       init_socket(cntrl, _endpoint + _cntrlport);
       init_socket(shell, _endpoint + _shellport);

       std::function<bool()> handlers[] = {
         [&cntrl]() {
           zmq::multipart_t msg;
           msg.recv(*cntrl);
           Rcpp::Rcout << "got cntrl msg" << std::endl;

           // special handling of control messages
           return true;
         },
         [&shell]() {
           zmq::multipart_t msg;
           msg.recv(*shell);
           Rcpp::Rcout << "got shell msg" << std::endl;
           // special handling of shell messages
           return true;
         }
       };
       poll(*_ctx, (zmq::socket_t* []){cntrl, shell}, handlers, 2);
     }

    ~JuniperKernel() {
      // set linger to 0 on all sockets
      // destroy sockets
      // destoy ctx
      signal();
      _hbthread.join();
      _iothread.join();

      if( _ctx ) {
        _stdin     ->setsockopt(ZMQ_LINGER, 0); delete _stdin;
        _inproc_sig->setsockopt(ZMQ_LINGER, 0); delete _inproc_sig;
        _inproc_pub->setsockopt(ZMQ_LINGER, 0); delete _inproc_pub;
        delete _ctx;
      }
    }
  
    void signal() {
       zmq::message_t request(0);
       memcpy (request.data (), "", 0);
       Rcpp::Rcout << "Sending kill " << std::endl;
       _inproc_sig->send(request);
    }

  private:
    // context is shared by all threads, cause there 
    // ain't no GIL to stop us now! ...we can build this thing together!
    zmq::context_t* const _ctx;

    // jupyter stdin
    zmq::socket_t*  const _stdin;

    // inproc sockets
    zmq::socket_t* const _inproc_pub;
    zmq::socket_t* const _inproc_sig;

    //misc
    std::string _endpoint;
    const std::string _hbport;
    const std::string _ioport;
    const std::string _shellport;
    const std::string _cntrlport;
    const std::string _key;
    const std::string _sig;

    std::thread _hbthread;
    std::thread _iothread;
};

// [[Rcpp::export]]
void boot_kernel(const std::string& connection_file) {
  signal(SIGSEGV, handler);

  JuniperKernel* jk = JuniperKernel::make(connection_file);
  jk->start_bg_threads();
  jk->run();
  while( 1 ) {
    sleep(1);
    // break;
    jk->signal();
    break;
  }
  delete jk;
}

// http://zguide.zeromq.org/page:all#Handling-Interrupt-Signals
// http://zguide.zeromq.org/page:all#Multithreading-with-ZeroMQ
