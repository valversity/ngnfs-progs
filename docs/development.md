## Running

Compile the code with make. You will need librcu and sparse installed.
The sparse package tends to depend on a lot of other packages, so you
may want to build and install your own.

ngnfs currently only runs in userspace. There are two components: the
devd process and the debugfs process.

The devd process listens on a socket for block IO requests. The
debugfs process reads commands from stdin and turns them into ngnfs
operations, which generate IO requests to the devd process. Thus you
need to tell the devd process where to listen and the debugfs process
where to connect. The devd process also needs to know where its
backing store is. Another required argument is a path to a file to
write trace data to.

Example invocation:

```
	$ truncate -s 1G ~/tmp/dev
	$ ./devd/ngnfs-devd -d ~/tmp/dev -l 127.0.0.1:8081 -t /tmp/trace_devd
```

In another terminal:

```
	$ ./cli/ngnfs-cli debugfs -d 127.0.0.1:8081 -t /tmp/trace_debugfs
	<1> $ mkfs
	<1> $ stat
```

## Code Layout

**{cli,devd}/**

These directories contain the source for utility binaries.  They'll have
their own private source files and will link with all the shared code.

**shared/**

This is all the code that's shared by each utility.  It's not a proper
library in that it doesn't need to remain API compatible with external
builds over time.

There's two kinds of shared code.  There's code that can run in either
userspace or the kernel (block.c) and shared code that only runs in
userspace (options.c).  It'd probably be worth making this distinction
more apparent.

**shared/lk/**

This is for userspace implementations of kernel interfaces.  This both
lets us use reasonably stand-alone kernel interfaces (list.h) in
userspace as well as share ngnfs code with the kernel module by
providing implementations of more complicated runtime services (RCU hash
tables, work queues).

**shared/format-{block,msg,trace}.h**

These headers contain the structures and protocol constants that are
exposed to the world through storage on persistent media or by sending
over the network.
