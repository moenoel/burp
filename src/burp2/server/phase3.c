#include "include.h"

#define WEAK_LEN	16
#define WEAK_STR_LEN	WEAK_LEN+1

static int copy_unchanged_entry(struct sbuf **csb, struct sbuf *sb,
	int *finished, struct blk **blk, struct manio *cmanio,
	struct manio *newmanio, const char *manifest_dir, struct conf *conf)
{
	static int ars;
	static char *copy;
	//static int sig_count=0;

	// Use the most recent stat for the new manifest.
	if(manio_write_sbuf(newmanio, sb)) return -1;

	if(!(copy=strdup((*csb)->path.buf)))
	{
		log_out_of_memory(__FUNCTION__);
		return -1;
	}

	while(1)
	{
		if((ars=manio_sbuf_fill(cmanio, *csb, *blk, NULL, conf))<0)
			return -1;
		else if(ars>0)
		{
			// Reached the end.
			*finished=1;
			sbuf_free(*csb); *csb=NULL;
			blk_free(*blk); *blk=NULL;
			free(copy);
			return 0;
		}
		// Got something.
		if(strcmp((*csb)->path.buf, copy))
		{
			// Found the next entry.
			free(copy);
			return 0;
		}

		// Should have the next signature.
		// Write it to the file.
		if(manio_write_sig_and_path(newmanio, *blk))
			break;
	}

	free(copy);
	return -1;
}

struct hooks
{
	char *path;
	char *fingerprints;
};

static int hookscmp(const struct hooks **a, const struct hooks **b)
{
	return strcmp((*a)->fingerprints, (*b)->fingerprints);
}

static int hooks_alloc(struct hooks **hnew, char **path, char **fingerprints)
{
	if(!*path || !*fingerprints) return 0;

	if(!(*hnew=(struct hooks *)malloc(sizeof(struct hooks))))
	{
		log_out_of_memory(__FUNCTION__);
		return -1;
	}
	
	(*hnew)->path=*path;
	(*hnew)->fingerprints=*fingerprints;
	*fingerprints=NULL;
	*path=NULL;
	return 0;
}

// Return 0 for OK, -1 for error, 1 for finished reading the file.
static int get_next_set_of_hooks(struct hooks **hnew, struct sbuf *sb,
	gzFile spzp, char **path, char **fingerprints,
	struct conf *conf)
{
	int ars;
	while(1)
	{
		if((ars=sbuf_fill_from_gzfile(sb, spzp, NULL, NULL, conf))<0)
			break;
		else if(ars>0)
		{
			// Reached the end.
			if(hooks_alloc(hnew, path, fingerprints))
				break;
			return 1;
		}
		if(sb->path.cmd==CMD_MANIFEST)
		{
			if(hooks_alloc(hnew, path, fingerprints))
				break;
			if(*fingerprints)
			{
				free(*fingerprints);
				*fingerprints=NULL;
			}
			if(*path) free(*path);
			*path=sb->path.buf;
			sb->path.buf=NULL;
			sbuf_free_content(sb);
			if(*hnew) return 0;
		}
		else if(sb->path.cmd==CMD_FINGERPRINT)
		{
			if(astrcat(fingerprints, sb->path.buf))
				break;
			sbuf_free_content(sb);
		}
		else
		{
			iobuf_log_unexpected(&sb->path, __FUNCTION__);
			break;
		}
	}

	return -1;
}

static int gzprintf_hooks(gzFile tzp, struct hooks *hooks)
{
	static char *f;
	static char ftmp[WEAK_STR_LEN];
	size_t len=strlen(hooks->fingerprints);

//	printf("NW: %c%04lX%s\n", CMD_MANIFEST,
//		strlen(hooks->path), hooks->path);
	gzprintf(tzp, "%c%04lX%s\n", CMD_MANIFEST,
		strlen(hooks->path), hooks->path);
	for(f=hooks->fingerprints; f<hooks->fingerprints+len; f+=WEAK_LEN)
	{
		snprintf(ftmp, sizeof(ftmp), "%s", f);
		gzprintf(tzp, "%c%04lX%s\n", CMD_FINGERPRINT,
			strlen(ftmp), ftmp);
	}
	return 0;
}

static void hooks_free(struct hooks **hooks)
{
	if(!*hooks) return;
	if((*hooks)->path) free((*hooks)->path);
	if((*hooks)->fingerprints) free((*hooks)->fingerprints);
	free(*hooks);
	*hooks=NULL;
}

static void try_lock_msg(int seconds)
{
	logp("Unable to get sparse lock for %d seconds.\n", seconds);
}

static int try_to_get_lock(struct lock *lock)
{
	// Sleeping for 1800*2 seconds makes 1 hour.
	// This should be super generous.
	int lock_tries=0;
	int lock_tries_max=1800;
	int sleeptime=2;

	while(1)
	{
		lock_get(lock);
		switch(lock->status)
		{
			case GET_LOCK_GOT:
				logp("Got sparse lock\n");
				return 0;
			case GET_LOCK_NOT_GOT:
				lock_tries++;
				if(lock_tries>lock_tries_max)
				{
					try_lock_msg(lock_tries_max*sleeptime);
					return -1;
				}
				// Log every 10 seconds.
				if(lock_tries%(10/sleeptime))
				{
					try_lock_msg(lock_tries_max*sleeptime);
					logp("Giving up.\n");
					return -1;
				}
				sleep(sleeptime);
				continue;
			case GET_LOCK_ERROR:
			default:
				logp("Unable to get global sparse lock.\n");
				return -1;
		}
	}
	// Never reached.
	return -1;
}

/* Merge two files of sorted sparse indexes into each other. */
static int merge_sparse_indexes(const char *srca, const char *srcb,
	const char *dst, struct conf *conf)
{
	int ars;
	int fcmp;
	int ret=-1;
	struct sbuf *asb=NULL;
	struct sbuf *bsb=NULL;
	char *afingerprints=NULL;
	char *bfingerprints=NULL;
	gzFile azp=NULL;
	gzFile bzp=NULL;
	gzFile dzp=NULL;
	struct hooks *anew=NULL;
	struct hooks *bnew=NULL;
	char *apath=NULL;
	char *bpath=NULL;

	if(!(asb=sbuf_alloc(conf))
	  || (srcb && !(bsb=sbuf_alloc(conf))))
		goto end;
	if(build_path_w(dst))
		goto end;
	if(!(azp=gzopen_file(srca, "rb"))
	  || (srcb && !(bzp=gzopen_file(srcb, "rb")))
	  || !(dzp=gzopen_file(dst, "wb")))
		goto end;

	while(azp || bzp || anew || bnew)
	{
		if(azp
		  && asb
		  && !anew
		  && (ars=get_next_set_of_hooks(&anew, asb, azp,
			&apath, &afingerprints, conf)))
		{
			if(ars<0) goto end;
			// ars==1 means it ended ok.
			gzclose_fp(&azp);
		}

		if(bzp
		  && bsb
		  && !bnew
		  && (ars=get_next_set_of_hooks(&bnew, bsb, bzp,
			&bpath, &bfingerprints, conf)))
		{
			if(ars<0) goto end;
			// ars==1 means it ended ok.
			gzclose_fp(&bzp);
		}

		if(anew && !bnew)
		{
			if(gzprintf_hooks(dzp, anew)) goto end;
			hooks_free(&anew);
		}
		else if(!anew && bnew)
		{
			if(gzprintf_hooks(dzp, bnew)) goto end;
			hooks_free(&bnew);
		}
		else if(!anew && !bnew)
		{
			continue;
		}
		else if(!(fcmp=hookscmp(
		  (const struct hooks **)&anew,
		  (const struct hooks **)&bnew)))
		{
			// They were the same - write the new one.
			if(gzprintf_hooks(dzp, bnew)) goto end;
			hooks_free(&anew);
			hooks_free(&bnew);
		}
		else if(fcmp<0)
		{
			if(gzprintf_hooks(dzp, anew)) goto end;
			hooks_free(&anew);
		}
		else
		{
			if(gzprintf_hooks(dzp, bnew)) goto end;
			hooks_free(&bnew);
		}
	}

	if(gzclose_fp(&dzp))
	{
		logp("Error closing %s in %s\n", tmpfile, __FUNCTION__);
		goto end;
	}

	ret=0;
end:
	gzclose_fp(&azp);
	gzclose_fp(&bzp);
	gzclose_fp(&dzp);
	sbuf_free(asb);
	sbuf_free(bsb);
	hooks_free(&anew);
	hooks_free(&bnew);
	if(afingerprints) free(afingerprints);
	if(bfingerprints) free(bfingerprints);
	if(apath) free(apath);
	if(bpath) free(bpath);
	return ret;
}

static int merge_into_global_sparse(const char *sparse, const char *global,
	struct conf *conf)
{
	int ret=-1;
	char *tmpfile=NULL;
	struct stat statp;
	char *lockfile=NULL;
	struct lock *lock=NULL;
	const char *globalsrc=NULL;
	
	if(!(tmpfile=prepend(global, "tmp", strlen("tmp"), ".")))
		goto end;

	// Get a lock before messing with the global sparse index.
	if(!(lockfile=prepend(global, "lock", strlen("lock"), "."))
	  || !(lock=lock_alloc_and_init(lockfile)))
		goto end;

	if(try_to_get_lock(lock)) goto end;

	if(!lstat(global, &statp)) globalsrc=global;

	if(merge_sparse_indexes(sparse, globalsrc, tmpfile, conf))
		goto end;

	if(do_rename(tmpfile, global)) goto end;

	ret=0;
end:
	lock_release(lock);
	lock_free(&lock);
	if(lockfile) free(lockfile);
	if(tmpfile) free(tmpfile);
	return ret;
}

static int sparse_generation(struct manio *newmanio, uint64_t fcount,
	const char *datadir, const char *manifest_dir, struct conf *conf)
{
	int ret=-1;
	uint64_t i=0;
	uint64_t pass=0;
	char *sparse=NULL;
	char *global_sparse=NULL;
	char *h1dir=NULL;
	char *h2dir=NULL;
	char *hooksdir=NULL;
	char *srca=NULL;
	char *srcb=NULL;
	char *dst=NULL;
	char compa[32]="";
	char compb[32]="";
	char compd[32]="";

	if(!(hooksdir=prepend_s(manifest_dir, "hooks"))
	  || !(h1dir=prepend_s(manifest_dir, "h1"))
	  || !(h2dir=prepend_s(manifest_dir, "h2")))
		goto end;

	while(1)
	{
		char *srcdir=NULL;
		char *dstdir=NULL;
		if(!pass)
		{
			srcdir=hooksdir;
			dstdir=h1dir;
		}
		else if(pass%2)
		{
			srcdir=h1dir;
			dstdir=h2dir;
		}
		else
		{
			srcdir=h2dir;
			dstdir=h1dir;
		}
		pass++;
		for(i=0; i<fcount; i+=2)
		{
			if(srca) { free(srca); srca=NULL; }
			if(srcb) { free(srcb); srcb=NULL; }
			if(dst) { free(dst); dst=NULL; }
			snprintf(compa, sizeof(compa), "%08lX", i);
			snprintf(compb, sizeof(compb), "%08lX", i+1);
			snprintf(compd, sizeof(compd), "%08lX", i/2);
			if(!(srca=prepend_s(srcdir, compa))
			  || !(dst=prepend_s(dstdir, compd)))
				goto end;
			if(i+1<fcount && !(srcb=prepend_s(srcdir, compb)))
				goto end;
			if(merge_sparse_indexes(srca, srcb, dst, conf))
				goto end;
		}
		if((fcount=i/2)<2) break;
	}

	if(!(sparse=prepend_s(manifest_dir, "sparse"))
	  || !(global_sparse=prepend_s(datadir, "sparse")))
		goto end;

	if(do_rename(dst, sparse)) goto end;

	if(merge_into_global_sparse(sparse, global_sparse, conf)) goto end;

	ret=0;
end:
	if(sparse) free(sparse);
	if(global_sparse) free(global_sparse);
	if(srca) free(srca);
	if(srcb) free(srcb);
	recursive_delete(h1dir, NULL, 1);
	recursive_delete(h2dir, NULL, 1);
	if(h1dir) free(h1dir);
	if(h2dir) free(h2dir);
	return ret;
}

// This is basically backup_phase3_server() from burp1. It used to merge the
// unchanged and changed data into a single file. Now it splits the manifests
// into several files.
int phase3(struct manio *chmanio, struct manio *unmanio,
	const char *manifest_dir, const char *datadir, struct conf *conf)
{
	int ars=0;
	int ret=1;
	int pcmp=0;
	char *hooksdir=NULL;
	char *dindexdir=NULL;
	struct sbuf *usb=NULL;
	struct sbuf *csb=NULL;
	struct blk *blk=NULL;
	int finished_ch=0;
	int finished_un=0;
	struct manio *newmanio=NULL;
	uint64_t fcount=0;

	logp("Start phase3\n");

	if(!(newmanio=manio_alloc())
	  || !(hooksdir=prepend_s(manifest_dir, "hooks"))
	  || !(dindexdir=prepend_s(manifest_dir, "dindex"))
	  || manio_init_write(newmanio, manifest_dir)
	  || manio_init_write_hooks(newmanio, conf->directory, hooksdir)
	  || manio_init_write_dindex(newmanio, dindexdir)
	  || !(usb=sbuf_alloc(conf))
	  || !(csb=sbuf_alloc(conf)))
		goto end;

	while(!finished_ch || !finished_un)
	{
		if(!blk && !(blk=blk_alloc())) return -1;

		if(!finished_un
		  && usb
		  && !usb->path.buf
		  && (ars=manio_sbuf_fill(unmanio, usb, NULL, NULL, conf)))
		{
			if(ars<0) goto end; // Error.
			finished_un=1; // OK.
		}

		if(!finished_ch
		  && csb
		  && !csb->path.buf
		  && (ars=manio_sbuf_fill(chmanio, csb, NULL, NULL, conf)))
		{
			if(ars<0) goto end; // Error.
			finished_ch=1; // OK.
		}

		if((usb && usb->path.buf) && (!csb || !csb->path.buf))
		{
			if(copy_unchanged_entry(&usb, usb, &finished_un,
				&blk, unmanio, newmanio, manifest_dir,
				conf)) goto end;
		}
		else if((!usb || !usb->path.buf) && (csb && csb->path.buf))
		{
			if(copy_unchanged_entry(&csb, csb, &finished_ch,
				&blk, chmanio, newmanio, manifest_dir,
				conf)) goto end;
		}
		else if((!usb || !usb->path.buf) && (!csb || !(csb->path.buf)))
		{
			continue;
		}
		else if(!(pcmp=sbuf_pathcmp(usb, csb)))
		{
			// They were the same - write one.
			if(copy_unchanged_entry(&csb, csb, &finished_ch,
				&blk, chmanio, newmanio, manifest_dir,
				conf)) goto end;
		}
		else if(pcmp<0)
		{
			if(copy_unchanged_entry(&usb, usb, &finished_un,
				&blk, unmanio, newmanio, manifest_dir,
				conf)) goto end;
		}
		else
		{
			if(copy_unchanged_entry(&csb, csb, &finished_ch,
				&blk, chmanio, newmanio, manifest_dir,
				conf)) goto end;
		}
	}

	fcount=newmanio->fcount;

	// Flush to disk and set up for reading.
	if(manio_set_mode_read(newmanio))
	{
		logp("Error setting %s to read in %s\n",
			newmanio->directory, __FUNCTION__);
		goto end;
	}

	if(sparse_generation(newmanio, fcount, datadir, manifest_dir, conf))
		goto end;

	recursive_delete(chmanio->directory, NULL, 1);
	recursive_delete(unmanio->directory, NULL, 1);

	ret=0;

	logp("End phase3\n");
end:
	manio_close(newmanio);
	manio_close(chmanio);
	manio_close(unmanio);
	sbuf_free(csb);
	sbuf_free(usb);
	blk_free(blk);
	if(hooksdir) free(hooksdir);
	if(dindexdir) free(dindexdir);
	return ret;
}
