/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * The management process and CLI handling
 */

#include "config.h"

#include <sys/utsname.h>

#include <stdarg.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "mgt/mgt.h"
#include "common/heritage.h"

#include "hash/hash_slinger.h"
#include "vav.h"
#include "vcli_serve.h"
#include "vev.h"
#include "vfil.h"
#include "vin.h"
#include "vpf.h"
#include "vrnd.h"
#include "vsha256.h"
#include "vsub.h"
#include "vtim.h"
#include "waiter/mgt_waiter.h"

struct heritage		heritage;
unsigned		d_flag = 0;
pid_t			mgt_pid;
struct vev_base		*mgt_evb;
int			exit_status = 0;
struct vsb		*vident;
struct VSC_C_mgt	static_VSC_C_mgt;
struct VSC_C_mgt	*VSC_C_mgt;

static struct vpf_fh *pfh = NULL;

int optreset;	// Some has it, some doesn't.  Cheaper than auto*

/*--------------------------------------------------------------------*/

static void
mgt_sltm(const char *tag, const char *sdesc, const char *ldesc)
{
	int i;

	assert(sdesc != NULL && ldesc != NULL);
	assert(*sdesc != '\0' || *ldesc != '\0');
	printf("\n%s\n", tag);
	i = strlen(tag);
	printf("%*.*s\n\n", i, i, "------------------------------------");
	if (*ldesc != '\0')
		printf("%s\n", ldesc);
	else if (*sdesc != '\0')
		printf("%s\n", sdesc);
}

/*lint -e{506} constant value boolean */
static void
mgt_DumpRstVsl(void)
{

	printf(
	    "\n.. The following is autogenerated output from "
	    "varnishd -x dumprstvsl\n\n");

#define SLTM(tag, flags, sdesc, ldesc) mgt_sltm(#tag, sdesc, ldesc);
#include "tbl/vsl_tags.h"
}

/*--------------------------------------------------------------------*/

static void
build_vident(void)
{
	struct utsname uts;

	vident = VSB_new_auto();
	AN(vident);
	if (!uname(&uts)) {
		VSB_printf(vident, ",%s", uts.sysname);
		VSB_printf(vident, ",%s", uts.release);
		VSB_printf(vident, ",%s", uts.machine);
	}
}

/*--------------------------------------------------------------------
 * 'Ello, I wish to register a complaint...
 */

#ifndef LOG_AUTHPRIV
#  define LOG_AUTHPRIV 0
#endif

const char C_ERR[] = "Error:";
const char C_INFO[] = "Info:";
const char C_DEBUG[] = "Debug:";
const char C_SECURITY[] = "Security:";
const char C_CLI[] = "Cli:";

void
MGT_complain(const char *loud, const char *fmt, ...)
{
	va_list ap;
	struct vsb *vsb;
	int sf;

	if (loud == C_CLI && !mgt_param.syslog_cli_traffic)
		return;
	vsb = VSB_new_auto();
	AN(vsb);
	va_start(ap, fmt);
	VSB_vprintf(vsb, fmt, ap);
	va_end(ap);
	AZ(VSB_finish(vsb));

	if (loud == C_ERR)
		sf = LOG_ERR;
	else if (loud == C_INFO)
		sf = LOG_INFO;
	else if (loud == C_DEBUG)
		sf = LOG_DEBUG;
	else if (loud == C_SECURITY)
		sf = LOG_WARNING | LOG_AUTHPRIV;
	else if (loud == C_CLI)
		sf = LOG_INFO;
	else
		WRONG("Wrong complaint loudness");

	if (loud != C_CLI)
		fprintf(stderr, "%s %s\n", loud, VSB_data(vsb));

	if (!MGT_DO_DEBUG(DBG_VTC_MODE))
		syslog(sf, "%s", VSB_data(vsb));
	VSB_destroy(&vsb);
}

/*--------------------------------------------------------------------*/

const void *
pick(const struct choice *cp, const char *which, const char *kind)
{

	for(; cp->name != NULL; cp++) {
		if (!strcmp(cp->name, which))
			return (cp->ptr);
	}
	ARGV_ERR("Unknown %s method \"%s\"\n", kind, which);
}

/*--------------------------------------------------------------------*/

static void
usage(void)
{
#define FMT "    %-28s # %s\n"

	fprintf(stderr, "usage: varnishd [options]\n");
	fprintf(stderr, FMT, "-a address[:port][,proto]",
	    "HTTP listen address and port (default: *:80)");
	fprintf(stderr, FMT, "", "  address: defaults to loopback");
	fprintf(stderr, FMT, "", "  port: port or service (default: 80)");
	fprintf(stderr, FMT, "", "  proto: HTTP/1 (default), PROXY");
	fprintf(stderr, FMT, "-b address[:port]", "backend address and port");
	fprintf(stderr, FMT, "", "  address: hostname or IP");
	fprintf(stderr, FMT, "", "  port: port or service (default: 80)");
	fprintf(stderr, FMT, "-C", "print VCL code compiled to C language");
	fprintf(stderr, FMT, "-d", "debug");
	fprintf(stderr, FMT, "-F", "Run in foreground");
	fprintf(stderr, FMT, "-f file", "VCL script");
	fprintf(stderr, FMT, "-h kind[,hashoptions]", "Hash specification");
	fprintf(stderr, FMT, "", "  -h critbit [default]");
	fprintf(stderr, FMT, "", "  -h simple_list");
	fprintf(stderr, FMT, "", "  -h classic");
	fprintf(stderr, FMT, "", "  -h classic,<buckets>");
	fprintf(stderr, FMT, "-i identity", "Identity of varnish instance");
	fprintf(stderr, FMT, "-j jail[,jailoptions]", "Jail specification");
#ifdef HAVE_SETPPRIV
	fprintf(stderr, FMT, "", "  -j solaris");
#endif
	fprintf(stderr, FMT, "", "  -j unix[,user=<user>][,ccgroup=<group>]");
	fprintf(stderr, FMT, "", "  -j none");
	fprintf(stderr, FMT, "-l vsl[,vsm]", "Size of shared memory file");
	fprintf(stderr, FMT, "", "  vsl: space for VSL records [80m]");
	fprintf(stderr, FMT, "", "  vsm: space for stats counters [1m]");
	fprintf(stderr, FMT, "-M address:port", "Reverse CLI destination");
	fprintf(stderr, FMT, "-n dir", "varnishd working directory");
	fprintf(stderr, FMT, "-P file", "PID file");
	fprintf(stderr, FMT, "-p param=value", "set parameter");
	fprintf(stderr, FMT,
	    "-r param[,param...]", "make parameter read-only");
	fprintf(stderr, FMT, "-S secret-file",
	    "Secret file for CLI authentication");
	fprintf(stderr, FMT,
	    "-s [name=]kind[,options]", "Backend storage specification");
	fprintf(stderr, FMT, "", "  -s malloc[,<size>]");
#ifdef HAVE_LIBUMEM
	fprintf(stderr, FMT, "", "  -s umem");
#endif
	fprintf(stderr, FMT, "", "  -s file,<dir_or_file>");
	fprintf(stderr, FMT, "", "  -s file,<dir_or_file>,<size>");
	fprintf(stderr, FMT, "",
	    "  -s file,<dir_or_file>,<size>,<granularity>");
	fprintf(stderr, FMT, "",
	    "  -s file,<dir_or_file>,<size>,<granularity>,<advice>");
	fprintf(stderr, FMT, "", "  -s persistent (experimental)");
	fprintf(stderr, FMT, "-T address:port",
	    "Telnet listen address and port");
	fprintf(stderr, FMT, "-t TTL", "Default TTL");
	fprintf(stderr, FMT, "-V", "version");
	fprintf(stderr, FMT, "-W waiter", "Waiter implementation");
#if defined(HAVE_KQUEUE)
	fprintf(stderr, FMT, "", "  -W kqueue");
#endif
#if defined(HAVE_PORT_CREATE)
	fprintf(stderr, FMT, "", "  -W ports");
#endif
#if defined(HAVE_EPOLL_CTL)
	fprintf(stderr, FMT, "", "  -W epoll");
#endif
	fprintf(stderr, FMT, "", "  -W poll");

#undef FMT
	exit(1);
}

/*--------------------------------------------------------------------*/

static void
cli_check(const struct cli *cli)
{
	if (cli->result == CLIS_OK) {
		VSB_clear(cli->sb);
		return;
	}
	AZ(VSB_finish(cli->sb));
	fprintf(stderr, "Error:\n%s\n", VSB_data(cli->sb));
	exit(2);
}

/*--------------------------------------------------------------------
 * All praise POSIX!  Thanks to our glorious standards there are no
 * standard way to get a back-trace of the stack, and even if we hack
 * that together from spit and pieces of string, there is no way no
 * standard way to translate a pointer to a symbol, which returns anything
 * usable.  (See for instance FreeBSD PR-134391).
 *
 * Attempt to run nm(1) on our binary during startup, hoping it will
 * give us a usable list of symbols.
 */

struct symbols {
	uintptr_t		a;
	uintptr_t		l;
	char			*n;
	VTAILQ_ENTRY(symbols)	list;
};

static VTAILQ_HEAD(,symbols) symbols = VTAILQ_HEAD_INITIALIZER(symbols);

int
Symbol_Lookup(struct vsb *vsb, void *ptr)
{
	struct symbols *s, *s0;
	uintptr_t pp;

	pp = (uintptr_t)ptr;
	s0 = NULL;
	VTAILQ_FOREACH(s, &symbols, list) {
		if (s->a > pp || s->a + s->l <= pp)
			continue;
		if (s0 == NULL || s->l < s0->l)
			s0 = s;
	}
	if (s0 == NULL)
		return (-1);
	VSB_printf(vsb, "%p: %s", ptr, s0->n);
	if ((uintmax_t)pp != s0->a)
		VSB_printf(vsb, "+0x%jx", (uintmax_t)pp - s0->a);
	return (0);
}

static void
Symbol_hack(const char *a0)
{
	char buf[BUFSIZ];
	FILE *fi;
	struct symbols *s;
	uintmax_t aa, ll;
	char type[10];
	char name[100];
	int i;

	bprintf(buf, "nm -t x -n -P %s 2>/dev/null", a0);
	fi = popen(buf, "r");
	if (fi == NULL)
		return;
	while (fgets(buf, sizeof buf, fi)) {
		i = sscanf(buf, "%99s\t%9s\t%jx\t%jx\n", name, type, &aa, &ll);
		if (i != 4)
			continue;
		s = malloc(sizeof *s + strlen(name) + 1);
		AN(s);
		s->a = aa;
		s->l = ll;
		s->n = (void*)(s + 1);
		strcpy(s->n, name);
		VTAILQ_INSERT_TAIL(&symbols, s, list);
	}
	(void)pclose(fi);
}

/*--------------------------------------------------------------------
 * This function is called when the CLI on stdin is closed.
 */

static void
cli_stdin_close(void *priv)
{

	(void)priv;

	if (d_flag) {
		mgt_stop_child();
		mgt_cli_close_all();
		if (pfh != NULL)
			(void)VPF_Remove(pfh);
		exit(0);
	} else {
		(void)close(0);
		(void)close(1);
		(void)close(2);
		AZ(open("/dev/null", O_RDONLY));
		assert(open("/dev/null", O_WRONLY) == 1);
		assert(open("/dev/null", O_WRONLY) == 2);
	}
}

/*--------------------------------------------------------------------
 * Autogenerate a -S file using strong random bits from the kernel.
 */

static void
mgt_secret_atexit(void)
{

	/* Only master process */
	if (getpid() != mgt_pid)
		return;
	VJ_master(JAIL_MASTER_FILE);
	(void)unlink("_.secret");
	VJ_master(JAIL_MASTER_LOW);
}

static const char *
make_secret(const char *dirname)
{
	char *fn;
	int fdo;
	int i;
	unsigned char b;

	assert(asprintf(&fn, "%s/_.secret", dirname) > 0);

	VJ_master(JAIL_MASTER_FILE);
	fdo = open(fn, O_RDWR|O_CREAT|O_TRUNC, 0640);
	if (fdo < 0)
		ARGV_ERR("Cannot create secret-file in %s (%s)\n",
		    dirname, strerror(errno));

	for (i = 0; i < 256; i++) {
		AZ(VRND_RandomCrypto(&b, 1));
		assert(1 == write(fdo, &b, 1));
	}
	AZ(close(fdo));
	VJ_master(JAIL_MASTER_LOW);
	AZ(atexit(mgt_secret_atexit));
	return (fn);
}

/*--------------------------------------------------------------------*/

static void
init_params(struct cli *cli)
{
	ssize_t def, low;

	MCF_CollectParams();

	MCF_TcpParams();

	if (sizeof(void *) < 8) {		/*lint !e506 !e774  */
		/*
		 * Adjust default parameters for 32 bit systems to conserve
		 * VM space.
		 */
		MCF_ParamConf(MCF_DEFAULT, "workspace_client", "24k");
		MCF_ParamConf(MCF_DEFAULT, "workspace_backend", "16k");
		MCF_ParamConf(MCF_DEFAULT, "http_resp_size", "8k");
		MCF_ParamConf(MCF_DEFAULT, "http_req_size", "12k");
		MCF_ParamConf(MCF_DEFAULT, "gzip_buffer", "4k");
		MCF_ParamConf(MCF_MAXIMUM, "vsl_space", "1G");
		MCF_ParamConf(MCF_MAXIMUM, "vsm_space", "1G");
	}

#if !defined(HAVE_ACCEPT_FILTERS) || defined(__linux)
	MCF_ParamConf(MCF_DEFAULT, "accept_filter", "off");
#endif

	low = sysconf(_SC_THREAD_STACK_MIN);
	MCF_ParamConf(MCF_MINIMUM, "thread_pool_stack", "%jdb", (intmax_t)low);

	def = 48 * 1024;
	if (def < low)
		def = low;
	MCF_ParamConf(MCF_DEFAULT, "thread_pool_stack", "%jdb", (intmax_t)def);

	MCF_ParamConf(MCF_MAXIMUM, "thread_pools", "%d", MAX_THREAD_POOLS);

	MCF_InitParams(cli);
}


/*--------------------------------------------------------------------*/

static void
identify(const char *i_arg)
{
	char id[17], *p;
	int i;

	strcpy(id, "varnishd");

	if (i_arg != NULL) {
		if (strlen(i_arg) + 1 > 1024)
			ARGV_ERR("Identity (-i) name too long (max 1023).\n");
		heritage.identity = strdup(i_arg);
		AN(heritage.identity);
		i = strlen(id);
		id[i++] = '/';
		for (; i < (sizeof(id) - 1L); i++) {
			if (!isalnum(*i_arg))
				break;
			id[i] = *i_arg++;
		}
		id[i] = '\0';
	}
	p = strdup(id);
	AN(p);

	openlog(p, LOG_PID, LOG_LOCAL0);
}

static void
mgt_tests(void)
{
	assert(VTIM_parse("Sun, 06 Nov 1994 08:49:37 GMT") == 784111777);
	assert(VTIM_parse("Sunday, 06-Nov-94 08:49:37 GMT") == 784111777);
	assert(VTIM_parse("Sun Nov  6 08:49:37 1994") == 784111777);

	/* Check that our SHA256 works */
	SHA256_Test();
}

static void
mgt_initialize(struct cli *cli)
{
	static unsigned clilim = 32768;

	/* for ASSERT_MGT() */
	mgt_pid = getpid();

	/* Create a cli for convenience in otherwise CLI functions */
	INIT_OBJ(cli, CLI_MAGIC);
	cli[0].sb = VSB_new_auto();
	AN(cli[0].sb);
	cli[0].result = CLIS_OK;
	cli[0].limit = &clilim;

	mgt_cli_init_cls();		// CLI commands can be registered

	init_params(cli);
	cli_check(cli);
}

static void
mgt_x_arg(const char *x_arg)
{
	if (!strcmp(x_arg, "dumprstparam"))
		MCF_DumpRstParam();
	else if (!strcmp(x_arg, "dumprstvsl"))
		mgt_DumpRstVsl();
	else if (!strcmp(x_arg, "dumprstcli"))
		mgt_DumpRstCli();
	else
		ARGV_ERR("Invalid -x argument\n");
}


/*--------------------------------------------------------------------*/

#define ERIC_MAGIC 0x2246988a		/* Eric is not random */

static int
mgt_eric(void)
{
	int eric_pipes[2];
	int fd;
	unsigned u;
	ssize_t sz;

	AZ(pipe(eric_pipes));

	switch (fork()) {
	case -1:
		fprintf(stderr, "Fork() failed: %s\n", strerror(errno));
		exit(-1);
	case 0:
		AZ(close(eric_pipes[0]));
		assert(setsid() > 1);

		fd = open("/dev/null", O_RDWR, 0);
		assert(fd > 0);
		assert(dup2(fd, STDIN_FILENO) == STDIN_FILENO);
		if (fd > STDIN_FILENO)
			AZ(close(fd));
		return (eric_pipes[1]);
	default:
		break;
	}
	AZ(close(eric_pipes[1]));
	sz = read(eric_pipes[0], &u, sizeof u);
	if (sz == sizeof u && u == ERIC_MAGIC)
		exit(0);
	else if (sz == sizeof u && u != 0)
		exit(u);
	else
		exit(-1);
}

static void
mgt_eric_im_done(int eric_fd, unsigned u)
{
	int fd;

	if (eric_fd < 0)
		return;

	if (u == 0)
		u = ERIC_MAGIC;

	fd = open("/dev/null", O_RDONLY);
	assert(fd >= 0);
	assert(dup2(fd, STDIN_FILENO) == STDIN_FILENO);
	AZ(close(fd));

	fd = open("/dev/null", O_WRONLY);
	assert(fd >= 0);
	assert(dup2(fd, STDOUT_FILENO) == STDOUT_FILENO);
	AZ(close(fd));

	fd = open("/dev/null", O_WRONLY);
	assert(fd >= 0);
	assert(dup2(fd, STDERR_FILENO) == STDERR_FILENO);
	AZ(close(fd));

	assert(write(eric_fd, &u, sizeof u) == sizeof u);
	AZ(close(eric_fd));
}

/*--------------------------------------------------------------------*/

int
main(int argc, char * const *argv)
{
	int o, eric_fd = -1;
	unsigned C_flag = 0;
	unsigned F_flag = 0;
	const char *b_arg = NULL;
	const char *f_arg = NULL;
	const char *i_arg = NULL;
	const char *j_arg = NULL;
	const char *h_arg = "critbit";
	const char *M_arg = NULL;
	const char *n_arg = NULL;
	const char *P_arg = NULL;
	const char *S_arg = NULL;
	const char *s_arg = "malloc,100m";
	const char *W_arg = NULL;
	const char *x_arg = NULL;
	int s_arg_given = 0;
	const char *T_arg = "localhost:0";
	char *p, *vcl = NULL;
	struct cli cli[1];
	char *dirname;
	char **av;
	char Cn_arg[] = "/tmp/varnishd_C_XXXXXXX";
	const char * opt_spec = "a:b:Cdf:Fh:i:j:l:M:n:P:p:r:S:s:T:t:VW:x:";
	unsigned u;

	mgt_tests();

	mgt_initialize(cli);

	/*
	 * First pass over arguments, to determine what we will be doing
	 * and what process configuration we will use for it.
	 */
	while ((o = getopt(argc, argv, opt_spec)) != -1) {
		switch (o) {
		case '?':
			usage();
		case 'b':
			b_arg = optarg;
			break;
		case 'C':
			C_flag = 1;
			break;
		case 'd':
			d_flag++;
			break;
		case 'f':
			f_arg = optarg;
			break;
		case 'F':
			F_flag = 1;
			break;
		case 'j':
			j_arg = optarg;
			break;
		case 'x':
			x_arg = optarg;
			break;
		default:
			break;
		}
	}

	if (argc != optind)
		ARGV_ERR("Too many arguments (%s...)\n", argv[optind]);

	if (x_arg != NULL) {
		if (argc != 3)
			ARGV_ERR("-x is incompatible with everything else\n");
		mgt_x_arg(x_arg);
		exit(0);
	}

	if (b_arg != NULL && f_arg != NULL)
		ARGV_ERR("Only one of -b or -f can be specified\n");

	if (d_flag && F_flag)
		ARGV_ERR("Only one of -d or -F can be specified\n");

	if (C_flag && b_arg == NULL && f_arg == NULL)
		ARGV_ERR("-C needs either -b <backend> or -f <vcl_file>\n");

	if (d_flag && C_flag)
		ARGV_ERR("-d makes no sense with -C\n");

	if (F_flag && C_flag)
		ARGV_ERR("-F makes no sense with -C\n");

	/*
	 * Start out by closing all unwanted file descriptors we might
	 * have inherited from sloppy process control daemons.
	 */
	VSUB_closefrom(STDERR_FILENO + 1);
	mgt_got_fd(STDERR_FILENO);

	setbuf(stdout, NULL);
	setbuf(stderr, NULL);

	/*
	 * Have Eric Daemonize us if need be
	 */
	if (!C_flag && !d_flag && !F_flag) {
		eric_fd = mgt_eric();
		mgt_got_fd(eric_fd);
		mgt_pid = getpid();
	}

	/* Set up the mgt counters */
	memset(&static_VSC_C_mgt, 0, sizeof static_VSC_C_mgt);
	VSC_C_mgt = &static_VSC_C_mgt;

	VRND_SeedAll();

	build_vident();

	Symbol_hack(argv[0]);

	/* Various initializations */
	VTAILQ_INIT(&heritage.socks);
	mgt_evb = vev_new_base();
	AN(mgt_evb);

	/* Initialize transport protocols */
	XPORT_Init();

	VJ_Init(j_arg);

	optind = 1;
	optreset = 1;
	while ((o = getopt(argc, argv, opt_spec)) != -1) {
		switch (o) {
		case 'b':
		case 'C':
		case 'd':
		case 'f':
		case 'F':
		case 'j':
		case 'x':
			/* Handled in first pass */
			break;
		case 'a':
			MAC_Arg(optarg);
			break;
		case 'h':
			h_arg = optarg;
			break;
		case 'i':
			i_arg = optarg;
			break;
		case 'l':
			av = VAV_Parse(optarg, NULL, ARGV_COMMA);
			AN(av);
			if (av[0] != NULL)
				ARGV_ERR("\t-l ...: %s\n", av[0]);
			if (av[1] != NULL) {
				MCF_ParamSet(cli, "vsl_space", av[1]);
				cli_check(cli);
			}
			if (av[1] != NULL && av[2] != NULL) {
				MCF_ParamSet(cli, "vsm_space", av[2]);
				cli_check(cli);
			}
			VAV_Free(av);
			break;
		case 'M':
			M_arg = optarg;
			break;
		case 'n':
			n_arg = optarg;
			break;
		case 'P':
			P_arg = optarg;
			break;
		case 'p':
			p = strchr(optarg, '=');
			if (p == NULL)
				usage();
			AN(p);
			*p++ = '\0';
			MCF_ParamSet(cli, optarg, p);
			*--p = '=';
			cli_check(cli);
			break;
		case 'r':
			MCF_ParamProtect(cli, optarg);
			cli_check(cli);
			break;
		case 'S':
			S_arg = optarg;
			break;
		case 's':
			s_arg_given = 1;
			STV_Config(optarg);
			break;
		case 'T':
			if (!strcmp(optarg, "none"))
				T_arg = NULL;
			else
				T_arg = optarg;
			break;
		case 't':
			MCF_ParamSet(cli, "default_ttl", optarg);
			break;
		case 'V':
			/* XXX: we should print the ident here */
			VCS_Message("varnishd");
			exit(0);
		case 'W':
			W_arg = optarg;
			break;
		default:
			usage();
		}
	}
	assert(argc == optind);

	if (C_flag) {
		if (n_arg == NULL) {
			AN(mkdtemp(Cn_arg));
			n_arg = Cn_arg;
		}
	}

	/* XXX: we can have multiple CLI actions above, is this enough ? */
	if (cli[0].result != CLIS_OK) {
		AZ(VSB_finish(cli[0].sb));
		ARGV_ERR("Failed parameter creation:\n%s\n",
		    VSB_data(cli[0].sb));
	}

	assert(d_flag == 0 || F_flag == 0);
	assert(b_arg == NULL || f_arg == NULL);

	if (S_arg != NULL && !strcmp(S_arg, "none")) {
		fprintf(stderr,
		    "Warning: CLI authentication disabled.\n");
	} else if (S_arg != NULL) {
		VJ_master(JAIL_MASTER_FILE);
		o = open(S_arg, O_RDONLY, 0);
		if (o < 0)
			ARGV_ERR("Cannot open -S file (%s): %s\n",
			    S_arg, strerror(errno));
		AZ(close(o));
		VJ_master(JAIL_MASTER_LOW);
	}

	if (f_arg != NULL) {
		vcl = VFIL_readfile(NULL, f_arg, NULL);
		if (vcl == NULL)
			ARGV_ERR("Cannot read -f file (%s): %s\n",
			    f_arg, strerror(errno));
	}

	if (VIN_N_Arg(n_arg, &heritage.name, &dirname, NULL) != 0)
		ARGV_ERR("Invalid instance (-n) name: %s\n", strerror(errno));

	identify(i_arg);

	if (VJ_make_workdir(dirname))
		ARGV_ERR("Cannot create working directory (%s): %s\n",
		    dirname, strerror(errno));

	VJ_master(JAIL_MASTER_FILE);
	if (P_arg && (pfh = VPF_Open(P_arg, 0644, NULL)) == NULL)
		ARGV_ERR("Could not open pid/lock (-P) file (%s): %s\n",
		    P_arg, strerror(errno));
	VJ_master(JAIL_MASTER_LOW);

	/* If no -s argument specified, process default -s argument */
	if (!s_arg_given)
		STV_Config(s_arg);

	/* Configure Transient storage, if user did not */
	STV_Config_Transient();

	mgt_vcl_init();

	if (b_arg != NULL || f_arg != NULL) {
		mgt_vcl_startup(cli, b_arg, f_arg, vcl, C_flag);
		if (C_flag) {
			if (Cn_arg == n_arg)
				(void)rmdir(Cn_arg);
			AZ(VSB_finish(cli->sb));
			fprintf(stderr, "%s\n", VSB_data(cli->sb));
			exit(cli->result == CLIS_OK ? 0 : 2);
		}
		cli_check(cli);
		free(vcl);
	}
	AZ(C_flag);

	if (VTAILQ_EMPTY(&heritage.socks))
		MAC_Arg(":80");

	assert(! VTAILQ_EMPTY(&heritage.socks));

	if (!d_flag) {
		if (b_arg == NULL && f_arg == NULL) {
			fprintf(stderr,
			    "Warning: Neither -b nor -f given,"
			    " won't start a worker child.\n"
			    "         Master process started,"
			    " use varnishadm to control it.\n");
		}
	}

	HSH_config(h_arg);

	Wait_config(W_arg);

	mgt_SHM_Init();

	AZ(VSB_finish(vident));

	if (S_arg == NULL)
		S_arg = make_secret(dirname);
	AN(S_arg);

	/**************************************************************
	 * After this point diagnostics will only be seen with -d
	 */

	assert(pfh == NULL || !VPF_Write(pfh));

	MGT_complain(C_DEBUG, "Platform: %s", VSB_data(vident) + 1);

	if (d_flag)
		mgt_cli_setup(0, 1, 1, "debug", cli_stdin_close, NULL);

	if (strcmp(S_arg, "none"))
		mgt_cli_secret(S_arg);

	if (M_arg != NULL)
		mgt_cli_master(M_arg);
	if (T_arg != NULL)
		mgt_cli_telnet(T_arg);

	/* Instantiate VSM */
	mgt_SHM_Create();

	u = MGT_Run();

	mgt_eric_im_done(eric_fd, u);

	o = vev_schedule(mgt_evb);
	if (o != 0)
		MGT_complain(C_ERR, "vev_schedule() = %d", o);

	MGT_complain(C_INFO, "manager dies");
	if (pfh != NULL)
		(void)VPF_Remove(pfh);
	exit(exit_status);
}
