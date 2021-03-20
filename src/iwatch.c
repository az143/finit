/* inotify watcher for files or directories
 *
 * Copyright (c) 2015-2016  Tobias Waldekranz <tobias@waldekranz.com>
 * Copyright (c) 2016-2021  Joachim Wiberg <troglobit@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <errno.h>

#include "finit.h"
#include "iwatch.h"
#include "log.h"

/*
 * iwatch is initialized and used mainly by the pidfile plugin, which is
 * one of the cornerstones in the condition subsystem.  Other parts of
 * Finit may use iwatch too, like env: watchers, but are disabled if the
 * pidfile plugin hasn't called iwatch_init() -- this is by design.
 */
static int initialized = 0;


int iwatch_init(struct iwatch *iw)
{
	if (!iw) {
		errno = EINVAL;
		return -1;
	}

	TAILQ_INIT(&iw->iwp_list);

	iw->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (iw->fd < 0) {
		_pe("Failed creating inotify descriptor");
		return -1;
	}
	initialized = 1;

	return iw->fd;
}

void iwatch_exit(struct iwatch *iw)
{
	struct iwatch_path *iwp, *tmp;

	TAILQ_FOREACH_SAFE(iwp, &iw->iwp_list, link, tmp) {
		TAILQ_REMOVE(&iw->iwp_list, iwp, link);
		inotify_rm_watch(iw->fd, iwp->wd);
		free(iwp->path);
		free(iwp);
	}

	close(iw->fd);
	initialized = 0;
}

int iwatch_add(struct iwatch *iw, char *file, uint32_t mask)
{
	struct iwatch_path *iwp;
	char *path;
	int wd;

	if (!initialized)
		return -1;

	if (!fexist(file)) {
		_d("skipping %s: no such file or directory", file);
		return 0;
	}
	_d("adding new watcher for path %s", file);

	path = strdup(file);
	if (!path) {
		_pe("Out of memory");
		return -1;
	}

	wd = inotify_add_watch(iw->fd, path, IWATCH_MASK | mask);
	if (wd < 0) {
		_pe("Failed adding watcher for %s", path);
		free(path);
		return -1;
	}

	iwp = malloc(sizeof(struct iwatch_path));
	if (!iwp) {
		_pe("Failed allocating new `struct iwatch_path`");
		inotify_rm_watch(iw->fd, wd);
		free(path);
		return -1;
	}

	iwp->path = path;
	iwp->wd = wd;
	TAILQ_INSERT_HEAD(&iw->iwp_list, iwp, link);

	return 0;
}

int iwatch_del(struct iwatch *iw, struct iwatch_path *iwp)
{
	if (!initialized)
		return -1;

	_d("Removing watcher for removed path %s", iwp->path);

	TAILQ_REMOVE(&iw->iwp_list, iwp, link);
	inotify_rm_watch(iw->fd, iwp->wd);
	free(iwp->path);
	free(iwp);

	return 0;
}

struct iwatch_path *iwatch_find_by_wd(struct iwatch *iw, int wd)
{
	struct iwatch_path *iwp;

	if (!initialized)
		return NULL;

	TAILQ_FOREACH(iwp, &iw->iwp_list, link) {
		if (iwp->wd == wd)
			return iwp;
	}

	return NULL;
}

struct iwatch_path *iwatch_find_by_path(struct iwatch *iw, const char *path)
{
	struct iwatch_path *iwp;

	if (!initialized)
		return NULL;

	TAILQ_FOREACH(iwp, &iw->iwp_list, link) {
		if (!strcmp(iwp->path, path))
			return iwp;
	}

	return NULL;
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */

