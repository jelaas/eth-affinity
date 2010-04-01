/*
 * File: aff.c
 * Implements: CPU affinity setting of ethernet devices
 *
 * Copyright: Jens Låås, UU 2009
 * Copyright license: According to GPL, see file COPYING in this directory.
 *
 */

#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <net/if.h>

#include "jelopt.h"
#include "jelist.h"

#define MAXCPU 64
#define MAX(a,b)  ((a)>(b) ? (a) : (b))

struct cpunode {
	int n; /* node number */
	int cpu[MAXCPU]; /* cpu vector. != 0 means cpu included in node */
};

struct dev {
	char *name, *fn, *old_affinity;
	int numa_node;
	int single, rr_multi, use_rps;
	int rps, rx, tx, txrx;
	int assigned_cpu;
	struct jlhead *rxq, *txq, *txrxq, *rpsq; // list of struct queue
};

struct queue {
	char *name, *fn, *old_affinity;
	int assigned_cpu;
	int n;
};

struct {
	char *procirq, *sysdir;
	int quiet, silent, dryrun, verbose, list, heuristics, reset;
	int maxcpu, reservedcpus;
	struct jlhead *limit, *exclude; /* list of char * */
	struct jlhead *devices; /* list if struct dev * */
	int rr_single, reserve_mq;
	int num_mq, max_rx, max_tx, max_txrx;
	struct jlhead *cpunodes;
} conf;

struct {
	int cur_cpu;
	int cur_mq_cpu;
	int nr_cpu;
	int nr_use_cpu;
	int cpu_offset;
	int rps_detected;
	int multinode;
} var;

const char *demask(const char *s);

/*
 * split a string into a list of strings.
 */
static struct jlhead *jl_splitstr(struct jlhead *l, const char *s, int delim)
{
	const char *start;
	char *path;
	
	while(*s)
	{
		for(start=s;*s;s++)
			if(*s == delim)
				break;
		path = malloc(s-start+1);
		strncpy(path, start, s-start);
		path[s-start] = 0;
		jl_append(l, path);
		if(*s) s++;
	}
	return l;
}

static struct cpunode *cpunode_get(int n)
{
	struct cpunode *node = NULL;
	
	jl_foreach(conf.cpunodes, node) {
		if(node->n == n)
			return node;
	}
	node = malloc(sizeof(struct cpunode));
	memset(node, 0, sizeof(struct cpunode));
	node->n = n;
	jl_append(conf.cpunodes, node);
	return node;
}

/* create a mask with all cpus on current node, except reserved CPUs */
static int node_cpu_mask(unsigned long long *bitmaskp, char *buf, size_t bufsize, int cpu)
{
	/* lookup node for cpu. then add all cpus from that node */
	struct cpunode *node, *usenode = NULL;
	unsigned long long bitmask = 0;
	int i;
	
	jl_foreach(conf.cpunodes, node) {
		if(node->cpu[cpu]) {
			usenode = node;
			break;
		}
	}
	if(!usenode)
		return -1;

	for(i=var.cpu_offset;i<MAXCPU;i++) {
		if(usenode->cpu[i])
			bitmask |= (1 << i);
	}

	if(bitmaskp) *bitmaskp = bitmask;
	snprintf(buf, bufsize, "%llx", bitmask);
	return 0;
}

static int reset_multiq(const struct dev *dev)
{
	int i;
	unsigned long long cpu = 0;
	char fn[256], buf[8];
	int fd, n;
	struct queue *q;
	
	for(i=0;i<var.nr_cpu;i++) {
		cpu |= (1 << i);
	}
	snprintf(buf, sizeof(buf), "%llx", cpu);
	
	for(i=0,q=jl_head_first(dev->rxq);i<dev->rx;i++,q=jl_next(q)) {
		snprintf(fn, sizeof(fn), "%s/smp_affinity", q->fn);
		if(!conf.quiet) {
			if(conf.verbose)
				printf("irq: cpu %s [mask 0x%s] -> %s %s\n",
				       demask(buf), buf, q->name, fn);
			else
				printf("irq %s -> %s\n", demask(buf), q->name);
		}

		if(!conf.dryrun) {
			fd = open(fn, O_WRONLY);
			if(fd == -1) {
				if(!conf.silent)
					fprintf(stderr,
						"Failed to open '%s'\n", fn);
				return -1;
			}
			
			n = strlen(buf);
			if(write(fd, buf, n)!=n) {
				close(fd);
				return -1;
			}
			close(fd);
		}		
	}

	for(i=0,q=jl_head_first(dev->txq);i<dev->tx;i++,q=jl_next(q)) {
		snprintf(fn, sizeof(fn), "%s/smp_affinity", q->fn);
		if(!conf.quiet) {
			if(conf.verbose)
				printf("irq: cpu %s [mask 0x%s] -> %s %s\n",
				       demask(buf), buf, q->name, fn);
			else
				printf("irq %s -> %s\n", demask(buf), q->name);
		}
		
		if(!conf.dryrun) {
			fd = open(fn, O_WRONLY);
			if(fd == -1) {
				if(!conf.silent)
					fprintf(stderr,
						"Failed to open '%s'\n", fn);
				return -1;
			}
			
			n = strlen(buf);
			if(write(fd, buf, n)!=n) {
				close(fd);
				return -1;
			}
			close(fd);
		}		
	}

	for(i=0,q=jl_head_first(dev->txrxq);i<dev->txrx;i++,q=jl_next(q)) {
		snprintf(fn, sizeof(fn), "%s/smp_affinity", q->fn);
		if(!conf.quiet) {
			if(conf.verbose)
				printf("irq: cpu %s [mask 0x%s] -> %s %s\n",
				       demask(buf), buf, q->name, fn);
			else
				printf("irq %s -> %s\n", demask(buf), q->name);
		}
		
		if(!conf.dryrun) {
			fd = open(fn, O_WRONLY);
			if(fd == -1) {
				if(!conf.silent)
					fprintf(stderr, 
						"Failed to open '%s'\n", fn);
				return -1;
			}
			
			n = strlen(buf);
			if(write(fd, buf, n)!=n) {
				close(fd);
				return -1;
			}
			close(fd);
		}		
	}

	for(i=0,q=jl_head_first(dev->rpsq);i<dev->rps;i++,q=jl_next(q)) {
		if(!conf.quiet) {
			if(conf.verbose)
				printf("rps: 00 -> %s %s\n", dev->name, q->fn);
			else
				printf("rps 00 -> %s\n", dev->name);
		}
		
		if(!conf.dryrun) {
			fd = open(q->fn, O_WRONLY);
			if(fd == -1) {
				if(!conf.silent)
					fprintf(stderr, 
						"Failed to open '%s'\n",
						q->fn);
				return -1;
			}
			write(fd, "00\n", 3);
			close(fd);
		}
	}

	return 0;
}

static int reset_singleq(const struct dev *dev)
{
	int i;
	unsigned long long cpu = 0;
	char fn[256], buf[8];
	int fd, n;
	struct queue *q;

	snprintf(fn, sizeof(fn), "%s/smp_affinity", dev->fn);
	
	for(i=0;i<var.nr_cpu;i++) {
		cpu |= (1 << i);
	}
	snprintf(buf, sizeof(buf), "%llx", cpu);

	if(!conf.quiet) {
		if(conf.verbose)
			printf("irq: cpu %s [mask 0x%s] -> %s %s\n",
			       demask(buf), buf, dev->name, fn);
		else
			printf("irq %s -> %s\n", demask(buf), dev->name);
	}
	
	if(!conf.dryrun) {
		fd = open(fn, O_WRONLY);
		if(fd == -1) {
			if(!conf.silent)
				fprintf(stderr,
					"Failed to open '%s'\n", fn);
			return -1;
		}
      
		n = strlen(buf);
		if(write(fd, buf, n)!=n) {
			close(fd);
			return -1;
		}
		close(fd);
	}

	for(i=0,q=jl_head_first(dev->rpsq);i<dev->rps;i++,q=jl_next(q)) {
		if(!conf.quiet) {
			if(conf.verbose)
				printf("rps: 00 -> %s %s\n", dev->name, q->fn);
			else
				printf("rps 00 -> %s\n", dev->name);
		}
		
		if(!conf.dryrun) {
			fd = open(q->fn, O_WRONLY);
			if(fd == -1) {
				if(!conf.silent)
					fprintf(stderr,
						"Failed to open '%s'\n", q->fn);
				return -1;
			}
			write(fd, "00\n", 3);
			close(fd);
		}
	}
	return 0;
}

/*
 * multiq interfaces gets queue = CPU affinity
 * If there are not enough CPUs we do round-robin
 *
 */
static int aff_multiq(const struct dev *dev)
{
	char fn[256], buf[8];
	int fd, n;
	int i, cpu;
	int rps_cpu = -1;
	struct queue *q;
	int cpu_offset, nr_use_cpu;

	cpu_offset = var.cpu_offset;
	nr_use_cpu = var.nr_use_cpu;	

	if( (!conf.reserve_mq) && ( (dev->rx+dev->txrx) >1) ) {
		nr_use_cpu = var.nr_cpu;
		cpu_offset = 0;
	}

	for(i=nr_use_cpu-cpu_offset,q=jl_head_first(dev->rxq);
	    q;
	    i++,q=jl_next(q)) {
		snprintf(fn, sizeof(fn), "%s/smp_affinity", q->fn);
		if(dev->rr_multi)
			cpu = (var.cur_mq_cpu++ % nr_use_cpu) + cpu_offset;
		else
			cpu = (i % nr_use_cpu) + cpu_offset;
		
		q->assigned_cpu = cpu;
		rps_cpu = cpu;
		
		snprintf(buf, sizeof(buf), "%x", 1 << cpu);
		
		if(!conf.quiet) {
			if(conf.verbose)
				printf("irq: cpu %d [mask 0x%s] -> %s@%d %s\n",
				       cpu, buf, q->name, dev->numa_node, fn);
			else
				printf("irq %d -> %s\n", cpu, q->name);
		}
		
		if(!conf.dryrun) {
			fd = open(fn, O_WRONLY);
			if(fd == -1) {
				if(!conf.silent)
					fprintf(stderr, 
						"Failed to open '%s'\n", fn);
				return -1;
			}
			
			n = strlen(buf);
			if(write(fd, buf, n)!=n) {
				close(fd);
				return -1;
			}
			close(fd);
		}
	}

	for(i=nr_use_cpu-cpu_offset,q=jl_head_first(dev->txq);
	    q;
	    i++,q=jl_next(q)) {
		snprintf(fn, sizeof(fn), "%s/smp_affinity", q->fn);
		cpu = (i % nr_use_cpu) + cpu_offset;

		/* single tx and rx queue: keep same cpu as for rx */
		if( (dev->tx == 1) && (dev->rx == 1) )
			cpu = rps_cpu;
		
		snprintf(buf, sizeof(buf), "%x", 1 << cpu);
		
		if(!conf.quiet) {
			if(conf.verbose)
				printf("irq: cpu %d [mask 0x%s] -> %s@%d %s\n",
				       cpu, buf, q->name, dev->numa_node, fn);
			else
				printf("irq %d -> %s\n", cpu, q->name);
		}
		
		if(!conf.dryrun) {
			fd = open(fn, O_WRONLY);
			if(fd == -1) {
				if(!conf.silent)
					fprintf(stderr, 
						"Failed to open '%s'\n", fn);
				return -1;
			}
			
			n = strlen(buf);
			if(write(fd, buf, n)!=n) {
				close(fd);
				return -1;
			}
			close(fd);
		}
	}

	for(i=nr_use_cpu-cpu_offset,q=jl_head_first(dev->txrxq);
	    q;
	    i++,q=jl_next(q)) {
		snprintf(fn, sizeof(fn), "%s/smp_affinity", q->fn);
		cpu = (i % nr_use_cpu) + cpu_offset;
		snprintf(buf, sizeof(buf), "%x", 1 << cpu);

		q->assigned_cpu = cpu;
		rps_cpu = cpu;
		
		if(!conf.quiet) {
			if(conf.verbose)
				printf("irq: cpu %d [mask 0x%s] -> %s@%d %s\n",
				       cpu, buf, q->name, dev->numa_node, fn);
			else
				printf("irq %d -> %s\n", cpu, q->name);
		}
		if(!conf.dryrun) {
			fd = open(fn, O_WRONLY);
			if(fd == -1) {
				if(!conf.silent)
					fprintf(stderr,
						"Failed to open '%s'\n", fn);
				return -1;
			}
			
			n = strlen(buf);
			if(write(fd, buf, n)!=n) {
				close(fd);
				return -1;
			}
			close(fd);
		}
	}

	if(dev->use_rps) {
		node_cpu_mask(NULL, buf, sizeof(buf), rps_cpu);
		jl_foreach(dev->rpsq, q) {
			if(!conf.quiet) {
				if(conf.verbose)
					printf("rps: cpu %s [mask 0x%s] -> %s@%d %s\n",
					       demask(buf),
					       buf,
					       q->name,
					       dev->numa_node,
					       q->fn);
				else
					printf("rps %s -> %s\n",
					       demask(buf), q->name);
			}
			
			if(!conf.dryrun) {
				fd = open(q->fn, O_WRONLY);
				if(fd == -1) {
					if(!conf.silent)
						fprintf(stderr,
							"Failed to open '%s'\n",
							q->fn);
					return -1;
				}
				n = strlen(buf);
				if(write(fd, buf, n)!=n) {
					close(fd);
					return -1;
				}
				close(fd);
			}
		}
	}
	
	return 0;
}

/*
 * singleq interfaces
 */
static int aff_singleq(struct dev *dev)
{
	char fn[256], buf[8];
	int fd, n, cpu;
	struct queue *q;
	
	snprintf(fn, sizeof(fn), "%s/smp_affinity", dev->fn);
	
	if(conf.rr_single)
		cpu = (var.cur_cpu++ % var.nr_use_cpu) + var.cpu_offset;
	else
		cpu = var.cpu_offset;

	dev->assigned_cpu = cpu;
	
	snprintf(buf, sizeof(buf), "%x", 1 << cpu);
	
	if(!conf.quiet) {
		if(conf.verbose)
			printf("irq: cpu %d [mask 0x%s] -> %s@%d %s\n", cpu, buf, dev->name, dev->numa_node, fn);
		else
			printf("irq %d -> %s\n", cpu, dev->name);
	}
	
	if(!conf.dryrun) {
		fd = open(fn, O_WRONLY);
		if(fd == -1) {
			if(!conf.silent)
				fprintf(stderr, "Failed to open '%s'\n", fn);
			return -1;
		}
      
		n = strlen(buf);
		if(write(fd, buf, n)!=n) {
			close(fd);
			return -1;
		}
		close(fd);
	}
	
	node_cpu_mask(NULL, buf, sizeof(buf), dev->assigned_cpu);
	if(dev->use_rps) {
		jl_foreach(dev->rpsq, q) {
			if(!conf.quiet) {
				if(conf.verbose)
					printf("rps: cpu %s [mask 0x%s] -> %s@%d %s\n",
					       demask(buf),
					       buf,
					       dev->name,
					       dev->numa_node,
					       q->fn);
				else
					printf("rps %s -> %s\n",
					       demask(buf), dev->name);
			}
			
			if(!conf.dryrun) {
				fd = open(q->fn, O_WRONLY);
				if(fd == -1) {
					if(!conf.silent)
						fprintf(stderr, 
							"Failed to open '%s'\n",
							q->fn);
					return -1;
				}
				n = strlen(buf);
				if(write(fd, buf, n)!=n) {
					close(fd);
					return -1;
				}
				close(fd);
			}
		}
	}

	return 0;
}

int dev_rx(const char *name)
{
	int q;
	char *p;
	
	p = strstr(name, "-rx-");
	if(p) {
		if(sscanf(p+4, "%d", &q)==1)
			return q+1;
	}
	return 0;
}

int dev_txrx(const char *name)
{
	int q;
	char *p;
	
	p = strstr(name, "-txrx-");
	if(!p) p = strstr(name, "-rxtx-");
	if(!p) p = strstr(name, "-TxRx-");
	if(p) {
		if(sscanf(p+6, "%d", &q)==1)
			return q+1;
	}
	return 0;
}

int dev_tx(const char *name)
{
	int q;
	char *p;
	
	p = strstr(name, "-tx-");
	if(p) {
		if(sscanf(p+4, "%d", &q)==1)
			return q+1;
	}
	return 0;
}

int qcmp(const void *i1, const void *i2)
{
	const struct queue *q1=i1, *q2=i2;
	
	return q1->n - q2->n;
}

int devcmp(const void *i1, const void *i2)
{
	const struct dev *d1=i1, *d2=i2;
	
	return strcmp(d1->name, d2->name);
}

struct dev *dev_get(struct jlhead *l, const char *dname)
{
	struct dev *dev;
	char name[16], *p, *ifname;
	int fd, n;
	char fn[256];
	char buf[8];
	
	strncpy(name, dname, 15);
	name[15] = 0;
	
	p = strchr(name, '-');
	if(p) *p=0;
	p = strchr(name, ':');
	if(p) *p=0;
	
	if(conf.exclude->len) {
		jl_foreach(conf.exclude, ifname) {
			if(!strcmp(ifname, name))
				return NULL;
		}
	}
  
	if(conf.limit->len) {
		jl_foreach(conf.limit, ifname) {
			if(!strcmp(ifname, name))
				goto ok;
		}
		return NULL;
	}
	
ok:
	jl_foreach(l, dev) {
		if(!strcmp(dev->name, name))
			return dev;
	}
	dev = malloc(sizeof(struct dev));
	if(dev) {
		memset(dev, 0, sizeof(struct dev));
		dev->name = strdup(name);
		dev->rxq = jl_new();
		dev->txq = jl_new();
		dev->txrxq = jl_new();
		dev->rpsq = jl_new();
		dev->assigned_cpu = -1;
		dev->use_rps = 0;
		jl_sort(dev->rxq, qcmp);
		jl_sort(dev->txq, qcmp);
		jl_sort(dev->txrxq, qcmp);
		if(jl_ins(l, dev))
			return NULL;

		sprintf(fn, "%s/class/net/%s/device/numa_node",
			conf.sysdir,
			dev->name);
		
		fd = open(fn, O_RDONLY);
		if(fd >= 0) {
			n = read(fd, buf, sizeof(buf));
			if(n > 0) {
				buf[n] = 0;
				dev->numa_node = atoi(buf);
			}
			close(fd);
		}

	}
	
	return dev;
}

struct queue *queue_new(const char *name, int n, const char *fn)
{
	struct queue *q;
	q = malloc(sizeof(struct queue));
	q->fn = strdup(fn);
	q->name = strdup(name);
	q->n = n;
	q->assigned_cpu = -1;
	return q;
}

int is_netdev(const char *name)
{
	char *p;

	name = strdup(name);
	if( (p = strchr(name, '-')) ) {
	  *p = 0;
	}

	/* this will work with namespaces too */
	return if_nametoindex(name);
}


int scan(struct jlhead *l, const struct dirent *ent, const char *base)
{
	DIR *d;
	char fn[256];
	struct dev *dev = NULL;
	int q;
	struct queue *queue = NULL;
	
	if(ent->d_name[0] == '.')
		return -1;
	
	snprintf(fn, sizeof(fn), "%s/%s", base, ent->d_name);
	
	d = opendir(fn);
	if(!d) return -1;
	
	while((ent = readdir(d))) {
		if(ent->d_name[0] == '.')
			continue;
		if(is_netdev(ent->d_name)) {
			dev = dev_get(l, ent->d_name);
			if(dev) {
				if((q=dev_rx(ent->d_name))) {
					dev->rx++;
					queue = queue_new(ent->d_name, q-1, fn);
					jl_ins(dev->rxq, queue);
					continue;
				} 
				if((q=dev_tx(ent->d_name))) {
					dev->tx++;
					queue = queue_new(ent->d_name,
							  q-1, fn);
					jl_ins(dev->txq, queue);
					continue;
				} 
				if((q=dev_txrx(ent->d_name))) {
					dev->txrx++;
					queue = queue_new(ent->d_name,
							  q-1, fn);
					jl_ins(dev->txrxq, queue);
					continue;
				}
				dev->fn = strdup(fn); /* pure dev irq */
			}
		}
	}
	closedir(d);
	
	if(dev) {
		char buf[8], afn[256];
		int fd, rc;
		
		snprintf(afn, sizeof(afn), "%s/smp_affinity", fn);
		fd = open(afn, O_RDONLY);
		if(fd != -1) {
			rc = read(fd, buf, sizeof(buf)-1);
			if(rc > 1) {
				buf[--rc] = 0;
				if(queue)
					queue->old_affinity = strdup(buf);
				else
					dev->old_affinity = strdup(buf);
			}
			close(fd);
		} else {
			if(queue)
				queue->old_affinity = "?";
			else
				dev->old_affinity = "?";
			if(!conf.silent)
				fprintf(stderr, 
					"Failed to read %s\n", afn);
			return -1;
		}
	}
  return 0;
}

/* /sys/devices/system/cpu/online */
static int cpu_online()
{
	int fd, n;
	char fn[256];
	char buf[16], *p;
	
	snprintf(fn, sizeof(fn), "%s/devices/system/cpu/online", conf.sysdir);
	
	fd = open(fn, O_RDONLY);
	if(fd == -1) return -1;
	n = read(fd, buf, sizeof(buf)-1);
	if(n < 1 ) return -1;
	
	close(fd);
	buf[n] = 0;
	p = strchr(buf, '-');
	if(!p) {
		/* single CPU system */
		var.nr_cpu = 1;
		return 0;
	}
	
	var.nr_cpu = atoi(p+1);
	var.nr_cpu++;
	
	return 0;
}


static int cpu_nodemap()
{
	struct stat statbuf;
	DIR *d;
	struct dirent *ent;
	int fd, n, i;
	struct cpunode *cpunode;
	char fn[512], buf[64];
	struct jlhead *intervals, *cpus;
	char *interval;
	int first,last;
	
        /* 
	   If "/sys/devices/system/node" exists we have a multinode system,
	   
	   For each "/sys/devices/system/node/nodeX" create a cpunode

	   Add cpus from /sys/devices/system/node/nodeX/cpulist to cpunode.
	   
	*/
	sprintf(fn, "%s/%s", conf.sysdir, "/devices/system/node");
	if(stat(fn, &statbuf)) {
		var.multinode = 0;
		cpunode = cpunode_get(0);
		for(i=0;i<var.nr_cpu;i++)
			cpunode->cpu[i] = 1;
		return 0;
	}
	
	var.multinode = 1;
	
	sprintf(fn, "%s/%s", conf.sysdir, "/devices/system/node");
	d = opendir(fn);	
	
	while((ent = readdir(d))) {
		if(strncmp(ent->d_name, "node", 4))
			continue;

		sprintf(fn, "%s/%s/%s/%s",
			conf.sysdir,
			"/devices/system/node",
			ent->d_name,
			"/cpulist");
		
		fd = open(fn, O_RDONLY);
		if(fd == -1)
			continue;

		cpunode = cpunode_get(atoi(ent->d_name+4));
		
		n = read(fd, buf, sizeof(buf));
		if(n > 0) {
			buf[n] = 0;
			/* parse buf: 0-3,8-11 */
			intervals = jl_new();
			jl_splitstr(intervals, buf, ',');
			jl_foreach(intervals, interval) {
				cpus = jl_new();
				jl_splitstr(cpus, interval, '-');
				first = atoi(jl_head_first(cpus));
				last = first;
				if(cpus->len > 1)
					last = atoi(jl_next(jl_head_first(cpus)));
				for(i=first;i<=last;i++)
					cpunode->cpu[i] = 1;
			}
		}
		close(fd);
	}
	closedir(d);

	return 0;
}	


int set_heuristics(struct jlhead *l)
{
	struct dev *dev;
	int only_sq = 1;
	int only_mq = 1;
	int exists_mq = 0;
	int exists_sq = 0;
	
	jl_foreach(l, dev) {
		if( (dev->rx > 1)||(dev->tx > 1)||(dev->txrx > 1) ) {
			exists_mq=1;
			conf.num_mq++;
			only_sq = 0;
			if(dev->rx > conf.max_rx) conf.max_rx = dev->rx;
			if(dev->tx > conf.max_tx) conf.max_tx = dev->tx;
			if(dev->txrx > conf.max_txrx) conf.max_txrx = dev->txrx;
		}
		if(dev->single) {
	                exists_sq=1;
			only_mq = 0;
		}
	}
	if(conf.max_txrx > conf.max_tx)
		conf.max_tx = conf.max_txrx;
	
	if(!conf.heuristics) return 0;
	
	/* turn on RPS if we have atleast one multiq interface or
	   we only have one interface */
	if(exists_mq || (l->len == 1))
		jl_foreach(l, dev) {
			if(dev->rps == 0)
				continue;
			
			/* only do RPS for devices with just 1 rx-queue */
			if(dev->rx == 1) {
				if(conf.verbose)
					printf("Heuristic:"
					       " RPS enabled for %s.\n",
					       dev->name);
				dev->use_rps = 1;
			}
		}
	
	if(exists_mq) {
		if(exists_sq)
			if(conf.verbose)
				printf("Heuristic:"
				       " round-robin affinity enabled "
				       "for all single-queue devices.\n");
		conf.rr_single = 1;
	} else {
		if( (var.nr_use_cpu > 1) && (l->len > 2) ) {
			if(conf.verbose)
				printf("Heuristic:"
				       " round-robin affinity enabled "
				       "for all single-queue devices.\n");
			conf.rr_single = 1;
		} else {
		if(conf.verbose)
			printf("Heuristic:"
			       " CPU 0 affinity for all "
			       "single-queue devices.\n");
		}
	}
	
	jl_foreach(l, dev) {
		if(!dev->single) {
			if((dev->txrx <= 1) && 
			   (dev->rx < var.nr_use_cpu) && 
			   (conf.num_mq > 1) && 
			   (dev->rx < conf.max_tx)) {
				if(conf.verbose)
					printf("Heuristic:"
					       " round-robin affinity enabled "
					       "for multi-queue device %s.\n",
					       dev->name);
				dev->rr_multi = 1;
			}
			if((dev->txrx > 1) && 
			   (dev->txrx < var.nr_use_cpu) && 
			   (conf.num_mq > 1) && 
			   (dev->txrx < conf.max_tx)) {
				if(conf.verbose)
					printf("Heuristic:"
					       " round-robin affinity enabled "
					       "for multi-queue device %s.\n",
					       dev->name);
				dev->rr_multi = 1;
			}
		}
	}

	return 0;
}

int detect_singleq(struct jlhead *l)
{
	struct dev *dev;
	
	/* fixup single queue devices */
	jl_foreach(l, dev) {
		if(dev->rx + dev->tx + dev->txrx == 0) {
			dev->single++;
			if(dev->rx == 0) {
				dev->rx = 1;
			}
			if(dev->tx == 0) {
				dev->tx = 1;
			}
		}
	}
	return 0;
}

int ins_comma_list(struct jlhead *l, char *ifnames)
{
	char *p, *end;
	
	for(p=ifnames;p&&*p;) {
		end = strchr(p, ',');
		if(end) *end = 0;
		jl_ins(l, strdup(p));
		p = end;
		if(p) p++;
	}
	return 0;
}

const char *demask(const char *s)
{
	unsigned long long n;
	long long i;
	size_t mlen=0;
	char buf[8], *out, *p;
	struct jlhead *l;
	l = jl_new();
	
	if(!s) return "?";
	if(*s == '?') return s;
	
	n = strtoull(s, NULL, 16);
	
	for(i=0;i<MAXCPU;i++)
	  if(n & ((unsigned long long)1<<i)) {
			snprintf(buf, sizeof(buf), "%lld", i);
			jl_append(l, strdup(buf));
		}
	
	jl_foreach(l, p)
		mlen += (strlen(p)+1);

	out = malloc(mlen+1);
	*out = 0;

	jl_foreach(l, p) {
		strcat(out, p);
		if(jl_next(p))
			strcat(out, ",");
	}
	if(l->len == 0)
		out = "na";
	
	return out;
}

	/*
	  Receive Packet Steering (RPS) support:
	  Anyway, current sysfs RPS interface exposes
	  a /sys/class/net/eth0/queues/rx-0/rps_cpus bitmap,

	  Distribute over CPU 0 and 1:
	  echo 03 > /sys/class/net/eth0/queues/rx-0/rps_cpus
	  (a multiq device would have more dirs under "queues", rx-1 etc)

	  So: just create a bitmap of all CPUs to be used and write it.

	  More advanced: only use CPUs from same node as irq-CPU.
	  We can record the irq affinty for singleq devices and only use
	  CPUs from the same node as the recorded one (dev->assigned_cpu).
	  See:
	  root@gatling:~# cat /sys/devices/system/node/node0/cpulist 
	  0-3

	  /sys/devices/system/node does not exist on single-node systems.

	  "We have found that masks that are cache aware (share same caches with
	  the interrupting CPU) mitigate much of this."

	*/
static int scan_rps()
{
	struct dev *dev;
	struct queue *queue;
	int n, fd, i;
	char fn[256], buf[64];
	
	jl_foreach(conf.devices, dev) {
		for(i=0;i<MAX(1, MAX(dev->rx, dev->txrx));i++) {
			snprintf(fn, sizeof(fn),
				 "%s/class/net/%s/queues/rx-%d/rps_cpus",
				 conf.sysdir,
				 dev->name,
				 i);
			
			fd = open(fn, O_RDONLY);
			if(fd == -1)
				continue;
			n = read(fd, buf, sizeof(buf)-1);
			if(n>1) {
				buf[--n] = 0;
				dev->rps++;
				queue = queue_new(dev->name,
						  i, fn);
				queue->old_affinity = strdup(buf);
				jl_ins(dev->rpsq, queue);
				
			}
			close(fd);
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	DIR *d;
	char *ifname;
	struct dirent *ent;
	struct dev *dev;
	int err=0;

	var.cur_cpu = 0;
	var.cur_mq_cpu = 0;
	var.nr_cpu = 1;
	var.nr_use_cpu = 1;
	var.cpu_offset = 0;
	
	conf.heuristics = 1;
	conf.procirq = "/proc/irq";
	conf.sysdir = "/sys";
	conf.limit = jl_new();
	conf.exclude = jl_new();
	conf.devices = jl_new();
	conf.cpunodes = jl_new();
	conf.reserve_mq = 1;
	
	jl_sort(conf.devices, devcmp);
	
	if(jelopt(argv, 'h', "help", NULL, NULL)) {
		printf("eth-affinity [-hvqstlR] [-r #] [-m #]\n"
		       " Version " VERSION " By Jens Låås, "
		       "UU 2009-2010.\n"
		       " Sets CPU affinity for ethernet devices.\n"
		       " Depends on information from sysfs and procfs.\n"
		       "\n"
		       " -v --verbose    Verbose output.\n"
		       " -q --quiet      Turn off output.\n"
		       " -s --silent     Turn off output including errors.\n"
		       " -t --test       Perform dryrun.\n"
		       " -l --list       Read and list current affinity.\n"
		       " -m --maxcpu N   Maximum nr of CPUs to use.\n"
		       "                 Excluding reserved CPUs.\n"
		       " -r --reserve N  Nr of CPUs to reserve (not use).\n"
		       "                 Reserves CPU 0-N.\n"
		       " -R --no-reserve-mq\n"
		       "                 Do not reserve CPUs for multiq devices.\n"
		       "                 Only takes effect when --reserve given.\n"
		       " -H --noheur     Disable heuristics.\n"
		       "                 Perform straight round-robin per device.\n"
		       " --reset         Reset affinity to all CPUs.\n"
		       " --devices N,..  Only configure these devices.\n"
		       " --exclude N,..  Do not configure these devices.\n"
		       " --sysdir DIR    [/sys]\n"
		       " --irqdir DIR    [/proc/irq]\n"
		       "\n"
			);
		exit(0);
	}
	if(jelopt(argv, 'q', "quiet", NULL, &err))
		conf.quiet = 1;
	if(jelopt(argv, 'l', "list", NULL, &err))
		conf.list = 1;
	if(jelopt(argv, 0, "reset", NULL, &err)) {
		conf.reset = 1;
		conf.heuristics = 0;
	}
	if(jelopt(argv, 's', "silent", NULL, &err))
		conf.quiet = conf.silent = 1;
	while(jelopt(argv, 'v', "verbose", NULL, &err))
		conf.verbose += 1;
	if(jelopt(argv, 't', "test", NULL, &err))
		conf.dryrun = 1;
	if(jelopt(argv, 'R', "no-reserve-mq", NULL, &err))
		conf.reserve_mq = 0;
	if(jelopt(argv, 'H', "noheur", NULL, &err))
		conf.heuristics = 0;
	if(jelopt(argv, 0, "sysdir", &conf.sysdir, &err))
		;
	if(jelopt(argv, 0, "irqdir", &conf.procirq, &err))
		;
	if(jelopt(argv, 0, "devices", &ifname, &err))
		ins_comma_list(conf.limit, ifname);
	if(jelopt(argv, 0, "exclude", &ifname, &err))
		ins_comma_list(conf.exclude, ifname);
	if(jelopt_int(argv, 'm', "maxcpu", &conf.maxcpu, &err))
		if(!conf.maxcpu) err |= 128;
	if(jelopt_int(argv, 'r', "reserve", &conf.reservedcpus, &err))
		;
	
	argc = jelopt_final(argv, &err);

	if(conf.quiet) conf.verbose = 0;
	
	if(err) {
		if(!conf.silent)
			fprintf(stderr, 
				"Syntax error in options.\n -h for help.\n");
		exit(1);
	}

	if(cpu_online()) {
		if(!conf.silent)
			fprintf(stderr,
				"Failed to read number of CPUs online from %s\n",
				conf.sysdir);
		exit(1);
	}
	var.nr_use_cpu = var.nr_cpu;

	cpu_nodemap();
	if(conf.verbose > 1) {
		struct cpunode *node;
		int i;
		
		jl_foreach(conf.cpunodes, node) {
			printf("Node: %d\n CPU: ", node->n);
			for(i=0;i<MAXCPU;i++)
				if(node->cpu[i]) printf("%d ", i);
			printf("\n");
		}
	}

	if(conf.maxcpu)
		if(var.nr_use_cpu > conf.maxcpu) {
			var.nr_use_cpu = conf.maxcpu;
		}
	
	var.cpu_offset = conf.reservedcpus;
	if(conf.reservedcpus) {
		while(var.cpu_offset+var.nr_use_cpu > var.nr_cpu) {
			var.nr_use_cpu--;
		}
		while(var.nr_use_cpu < 1) {
			var.cpu_offset--;
			var.nr_use_cpu++;
		}
		if(var.cpu_offset < 0)
			var.cpu_offset = 0;
	}
	var.cur_mq_cpu = var.nr_cpu - var.cpu_offset;
	
	d = opendir(conf.procirq);
	if(!d) {
		if(!conf.silent)
			fprintf(stderr, "Failed to open %s\n",
				conf.procirq);
		exit(1);
	}

	while((ent = readdir(d))) {
		scan(conf.devices, ent, conf.procirq);
	}
	closedir(d);

	detect_singleq(conf.devices);
	
	scan_rps();

	if(conf.verbose > 1) {
		jl_foreach(conf.devices, dev) {
			printf("%s queues:", dev->name);
			printf(" rx=%d", dev->rx);
			printf(" tx=%d", dev->tx);
			printf(" txrx=%d", dev->txrx);
			printf(" rps=%d\n", dev->rps);
		}
	}
	
	if(conf.list) {
		jl_foreach(conf.devices, dev) {
			if(dev->single) {
				struct queue *q;
				if(conf.verbose)
					printf("irq: cpu %s [mask 0x%s] -> %s@%d\n",
					       demask(dev->old_affinity),
					       dev->old_affinity, dev->name,
					       dev->numa_node);
				else
					printf("irq %s -> %s\n",
					       demask(dev->old_affinity),
					       dev->name);
				for(q=jl_head_first(dev->rpsq);q;q=jl_next(q)) {
					if(conf.verbose)
						printf("rps: cpu %s [mask 0x%s] -> %s@%d\n",
						       demask(q->old_affinity),
						       q->old_affinity,
						       q->name,
						       dev->numa_node);
					else
						printf("rps %s -> %s\n",
						       demask(q->old_affinity),
						       q->name);
				}
			} else {
				struct queue *q;
				
				for(q=jl_head_first(dev->rxq);q;q=jl_next(q)) {
					if(conf.verbose)
						printf("irq: cpu %s [mask 0x%s] -> %s@%d\n",
						       demask(q->old_affinity),
						       q->old_affinity,
						       q->name,
						       dev->numa_node);
					else
						printf("irq %s -> %s\n",
						       demask(q->old_affinity),
						       q->name);
				}
				for(q=jl_head_first(dev->txq);q;q=jl_next(q)) {
					if(conf.verbose)
						printf("irq: cpu %s [mask 0x%s] -> %s\n",
						       demask(q->old_affinity),
						       q->old_affinity, q->name);
					else
						printf("irq %s -> %s\n",
						       demask(q->old_affinity),
						       q->name);
				}
				for(q=jl_head_first(dev->txrxq);q;q=jl_next(q)) {
					if(conf.verbose)
						printf("irq: cpu %s [mask 0x%s] -> %s@%d\n",
						       demask(q->old_affinity),
						       q->old_affinity,
						       q->name,
						       dev->numa_node);
					else
						printf("irq %s -> %s\n",
						       demask(q->old_affinity),
						       q->name);
				}
				if(dev->rx == 1)
				for(q=jl_head_first(dev->rpsq);q;q=jl_next(q)) {
					if(conf.verbose)
						printf("rps: cpu %s [mask 0x%s] -> %s@%d\n",
						       demask(q->old_affinity),
						       q->old_affinity,
						       q->name,
						       dev->numa_node);
					else
						printf("rps %s -> %s\n",
						       demask(q->old_affinity),
						       q->name);
				}
			}
		}
		exit(0);
	}
	
	set_heuristics(conf.devices);

	if(conf.reset) {
		jl_foreach(conf.devices, dev) {
			if(dev->single)
				reset_singleq(dev);
			else
				reset_multiq(dev);
		}
	}

	if(!conf.reset)
		jl_foreach(conf.devices, dev) {
			if(dev->single)
				aff_singleq(dev);
			else
				aff_multiq(dev);
		}
	
	exit(0);
}
