#ifndef libpstack_proc_h
#define libpstack_proc_h
#include <elf.h>
#include <memory.h>
extern "C" {
// Some thread_db headers are not safe to include unwrapped in extern "C"
#include <thread_db.h>
}

#include <map>
#include <set>
#include <sstream>
#include <functional>
#include <bitset>

#include "libpstack/ps_callback.h"
#include "libpstack/dwarf.h"

struct ps_prochandle {};

class Process;

namespace Dwarf {
class StackFrame;
class ExpressionStack : public std::stack<Elf::Addr> {
public:
    bool isReg;
    int inReg;

    ExpressionStack(): isReg(false) {}
    Elf::Addr poptop() { Elf::Addr tos = top(); pop(); return tos; }
    Elf::Addr eval(const Process &, DWARFReader &r, const StackFrame*, Elf::Addr);
    Elf::Addr eval(const Process &, const DIE::Attribute &, const StackFrame*, Elf::Addr);
};

// this works for i386 and x86_64 - might need to change for other archs.
typedef unsigned long cpureg_t;

/*
 * The unwind mechanism tells us how this stack frame was created
 */
enum class UnwindMechanism {
   // this frame was created from machine state - it's the "top of stack".
   MACHINEREGS,

   // created by using DWARF unwinding information from previous.
   DWARF,

   // frame pointer register in previous frame.
   FRAMEPOINTER,

   // attempt was made to recover stack state by assuming the previous frame
   // was target of a call to a bad address
   BAD_IP_RECOVERY,

   // The previous frame was a signal "trampoline" - On receipt of a signal,
   // the kernel saved the processor state on the stack, and arranged for the
   // previous frame to be invoked. Unwinding requires decoding the register
   // state stored by the kernel on the stack.
   TRAMPOLINE,

   // The stack frame was built up by scanning a log file.
   LOGFILE,

   INVALID,
};

class StackFrame {
    mutable Dwarf::DIE function_;
public:
    Dwarf::DIE &function() const;
    Elf::Addr rawIP() const;
    Elf::Addr scopeIP() const;
    Elf::Addr cfa;
    std::map<unsigned, cpureg_t> regs;
    Elf::Object::sptr elf;
    Elf::Addr elfReloc;
    const Elf::Phdr *phdr;
    Info::sptr dwarf;
    CFI *frameInfo;
    const FDE *fde;
    const CIE *cie;
    UnwindMechanism mechanism;
    StackFrame() : StackFrame(UnwindMechanism::INVALID) {}
    StackFrame(UnwindMechanism mechanism)
        : cfa(0)
        , elfReloc(0)
        , phdr(0)
        , dwarf(0)
        , frameInfo(0)
        , fde(0)
        , cie(0)
        , mechanism(mechanism)
    {}
    StackFrame &operator = (const StackFrame &) = delete;
    void setReg(unsigned, cpureg_t);
    cpureg_t getReg(unsigned regno) const;
    Elf::Addr getCFA(const Process &, const CallFrame &) const;
    bool unwind(Process &p, StackFrame &out);
    void setCoreRegs(const Elf::CoreRegisters &);
    void getCoreRegs(Elf::CoreRegisters &) const;
    void getFrameBase(const Process &, intmax_t, ExpressionStack *) const;
    void findObjectCode(Process &);
};
}

struct ThreadStack {
    td_thrinfo_t info;
    std::vector<Dwarf::StackFrame> stack;
    ThreadStack() {
        memset(&info, 0, sizeof info);
    }
    void unwind(Process &, Elf::CoreRegisters &regs, unsigned maxFrames);
};


struct PstackOptions {
   bool nosrc = false;
   bool doargs = false;
   bool dolocals = false;
   bool nothreaddb = false;
   std::vector<std::pair<std::string, std::string>> pathReplacements;
   int maxdepth = std::numeric_limits<int>::max();
};

/*
 * This contains information about an LWP.  In linux, since NPTL, this is
 * essentially a thread. Old style, userland threads may have a single LWP for
 * all threads.
 */
struct Lwp {
    int stopCount;
    timeval stoppedAt;
    Lwp() : stopCount{0}, stoppedAt{0,0} {}
};

struct PrintableFrame;


struct ProcessLocation {
};

struct AddressRange {
   Elf::Addr start;
   Elf::Addr fileSize;
   Elf::Addr memSize;
   AddressRange( Elf::Addr start_, Elf::Addr fileSize_, Elf::Addr memSize_ ) : start(start_), fileSize(fileSize_), memSize(memSize_) {}
   // std::string map;
   // long flags; .... add protection flags, etc.
};

class Process : public ps_prochandle {
    Elf::Addr findRDebugAddr();
    Elf::Addr entry;
    Elf::Addr interpBase;
    void loadSharedObjects(Elf::Addr);
    Elf::Addr vdsoBase;

protected:
    virtual bool loadSharedObjectsFromFileNote() = 0;
    td_thragent_t *agent;
    Elf::Object::sptr execImage;
    Elf::Object::sptr vdsoImage;
    std::string abiPrefix;
    PstackOptions options;

public:
    Elf::Addr sysent; // for AT_SYSINFO
    std::map<pid_t, Lwp> lwps;
    Dwarf::ImageCache &imageCache;
    std::map<Elf::Addr, Elf::Object::sptr> objects;
    void processAUXV(const Reader &);
    Reader::sptr io;

    virtual bool getRegs(lwpid_t pid, Elf::CoreRegisters *reg) = 0;
    void addElfObject(Elf::Object::sptr obj, Elf::Addr load);
    // Find the the object (and its load address) and segment containing a given address
    std::tuple<Elf::Addr, Elf::Object::sptr, const Elf::Phdr *> findSegment(Elf::Addr addr) const;
    Dwarf::Info::sptr getDwarf(Elf::Object::sptr);
    Process(Elf::Object::sptr exec, Reader::sptr memory, const PstackOptions &prl, Dwarf::ImageCache &cache);
    virtual void stop(pid_t lwpid) = 0;
    virtual void stopProcess() = 0;
    virtual void resumeProcess() = 0;
    virtual void resume(pid_t lwpid) = 0;
    std::ostream &dumpStackText(std::ostream &, const ThreadStack &, const PstackOptions &) const;
    std::ostream &dumpFrameText(std::ostream &, const PrintableFrame &, Dwarf::StackFrame &) const;
    std::ostream &dumpStackJSON(std::ostream &, const ThreadStack &) const;
    template <typename T> void listThreads(const T &);


    // find address of named symbol in the process.
    Elf::Addr resolveSymbol(const char *symbolName, bool includeDebug,
          std::function<bool(Elf::Addr, const Elf::Object::sptr &)> matcher = [](Elf::Addr, const Elf::Object::sptr &) { return true; }) const;

    // find symbol data of named symbol in the process.
    // like resolveSymbol, but return the library and that library's load address as well as the address in the process.
    std::tuple<Elf::Object::sptr, Elf::Addr, Elf::Addr>
    resolveSymbolDetail(const char *name, bool includeDebug,
                        std::function<bool(Elf::Addr, const Elf::Object::sptr&)> match =
                           [](Elf::Addr, const Elf::Object::sptr &) { return true; }) const;
    virtual std::list<ThreadStack> getStacks(const PstackOptions &, unsigned maxFrames);
    virtual ~Process();
    virtual void load(const PstackOptions &);
    virtual pid_t getPID() const = 0;
    virtual std::vector<AddressRange> addressSpace() const = 0;
    static std::shared_ptr<Process> load(Elf::Object::sptr exe, std::string id, const PstackOptions &options, Dwarf::ImageCache &cache);
};

template <typename T> int
threadListCb(const td_thrhandle_t *thr, void *v)
{ T &callback = *(T *)v; callback(thr); return 0; }

template <typename T> void
Process::listThreads(const T &callback)
{
    td_ta_thr_iter(agent,
            threadListCb<T>,
            (void *)&callback, TD_THR_ANY_STATE, TD_THR_LOWEST_PRIORITY, TD_SIGNO_MASK, TD_THR_ANY_USER_FLAGS);
}

class LiveReader : public FileReader {
public:
    Off size() const override { return std::numeric_limits<Off>::max(); }
    LiveReader(pid_t, const std::string &);
};

// Name of the file /proc/<pid>/name, after symlink dereferencing
std::string procname(pid_t pid, const std::string &);

struct LiveThreadList;
class LiveProcess : public Process {
    pid_t pid;
public:
    LiveProcess(Elf::Object::sptr &, pid_t, const PstackOptions &, Dwarf::ImageCache &);
    virtual bool getRegs(lwpid_t pid, Elf::CoreRegisters *reg) override;
    virtual void stop(pid_t) override;
    virtual void resume(pid_t) override;
    void stopProcess() override;
    void resumeProcess() override;
    virtual void load(const PstackOptions &) override;
    void findLWPs();
    virtual pid_t getPID() const override;
protected:
    bool loadSharedObjectsFromFileNote() override;
    std::vector<AddressRange> addressSpace() const override;
};

class CoreProcess;
class CoreReader : public Reader {
    Process *p;
    Elf::Object::sptr core;
protected:
    virtual size_t read(Off remoteAddr, size_t size, char *ptr) const override;
public:
    CoreReader (Process *, Elf::Object::sptr);
    virtual void describe(std::ostream &os) const override;
    Off size() const override { return std::numeric_limits<Off>::max(); }
    std::string filename() const override { return "process memory"; }
};

class CoreProcess : public Process {
public:
    Elf::Object::sptr coreImage;
    CoreProcess(Elf::Object::sptr exec, Elf::Object::sptr core, const PstackOptions &, Dwarf::ImageCache &);
    virtual bool getRegs(lwpid_t pid, Elf::CoreRegisters *reg) override;
    virtual void stop(lwpid_t) override;
    virtual void resume(lwpid_t) override;
    void stopProcess() override;
    void resumeProcess()  override { }
    void findLWPs();
    virtual void load(const PstackOptions &) override;
    virtual pid_t getPID() const override;
protected:
    bool loadSharedObjectsFromFileNote() override;
    std::vector<AddressRange> addressSpace() const override;
};

// RAII to stop a process.
struct StopProcess {
    Process *proc;
public:
    StopProcess(Process *proc_) : proc(proc_) { proc->stopProcess(); }
    void clear() {
        if (proc) {
            proc->resumeProcess();
            proc = nullptr;
        }
    }
    ~StopProcess() { clear(); }
};

// RAII to stop a thread.
struct StopLWP {
    Process *proc;
    lwpid_t lwp;
public:
    StopLWP(Process *proc_, lwpid_t lwp_) : proc(proc_), lwp(lwp_) { proc->stop(lwp); }
    ~StopLWP() { proc->resume(lwp); }
};
class LogProcess : public Process {
   const std::vector<std::string> &logs;
   std::list<ThreadStack> stacks;
public:
   LogProcess(Elf::Object::sptr exec, const std::vector<std::string> &logs, const PstackOptions &, Dwarf::ImageCache &);
   void load(const PstackOptions &);
   bool getRegs(lwpid_t, Elf::CoreRegisters *);
   void resume(pid_t);
   void resumeProcess();
   void stop(lwpid_t);
   void stopProcess();
   std::vector<AddressRange> addressSpace() const;
   pid_t getPID() const;
   bool loadSharedObjectsFromFileNote();
   virtual std::list<ThreadStack> getStacks(const PstackOptions &, unsigned maxFrames);
};

#endif
