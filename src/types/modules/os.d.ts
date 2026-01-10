declare module 'os' {
  interface CpuInfo {
    model: string;
    speed: number;
    times: {
      user: number;
      nice: number;
      sys: number;
      idle: number;
      irq: number;
    };
  }

  interface NetworkInterfaceInfo {
    address: string;
    netmask: string;
    family: 'IPv4' | 'IPv6';
    mac: string;
    internal: boolean;
    cidr: string | null;
  }

  interface UserInfo {
    username: string;
    uid: number;
    gid: number;
    shell: string;
    homedir: string;
  }

  interface OsConstants {
    signals: {
      SIGHUP?: number;
      SIGINT?: number;
      SIGQUIT?: number;
      SIGILL?: number;
      SIGTRAP?: number;
      SIGABRT?: number;
      SIGIOT?: number;
      SIGBUS?: number;
      SIGFPE?: number;
      SIGKILL?: number;
      SIGUSR1?: number;
      SIGUSR2?: number;
      SIGSEGV?: number;
      SIGPIPE?: number;
      SIGALRM?: number;
      SIGTERM?: number;
      SIGCHLD?: number;
      SIGCONT?: number;
      SIGSTOP?: number;
      SIGTSTP?: number;
      SIGTTIN?: number;
      SIGTTOU?: number;
      SIGURG?: number;
      SIGXCPU?: number;
      SIGXFSZ?: number;
      SIGVTALRM?: number;
      SIGPROF?: number;
      SIGWINCH?: number;
      SIGIO?: number;
      SIGSYS?: number;
    };
    errno: Record<string, number>;
    priority: {
      PRIORITY_LOW: number;
      PRIORITY_BELOW_NORMAL: number;
      PRIORITY_NORMAL: number;
      PRIORITY_ABOVE_NORMAL: number;
      PRIORITY_HIGH: number;
      PRIORITY_HIGHEST: number;
    };
    dlopen: {
      RTLD_LAZY?: number;
      RTLD_NOW?: number;
      RTLD_GLOBAL?: number;
      RTLD_LOCAL?: number;
      RTLD_DEEPBIND?: number;
    };
    UV_UDP_REUSEADDR: number;
  }

  const EOL: string;
  const devNull: string;
  const constants: OsConstants;

  function arch(): string;
  function platform(): string;
  function type(): string;
  function release(): string;
  function version(): string;
  function machine(): string;
  function hostname(): string;
  function homedir(): string;
  function tmpdir(): string;
  function endianness(): 'BE' | 'LE';
  function uptime(): number;
  function totalmem(): number;
  function freemem(): number;
  function availableParallelism(): number;
  function cpus(): CpuInfo[];
  function loadavg(): [number, number, number];
  function networkInterfaces(): Record<string, NetworkInterfaceInfo[]>;
  function userInfo(): UserInfo;
  function getPriority(pid?: number): number;
  function setPriority(priority: number): void;
  function setPriority(pid: number, priority: number): void;
}
