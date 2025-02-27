#include "git-compat-util.h"
#include "environment.h"
#include "gettext.h"
#include "trace2.h"
#include "config.h"
#include "dir.h"
#include "hashmap.h"
#include "run-command.h"
#include "name-hash.h"
#include "read-cache-ll.h"
#include "virtualfilesystem.h"

#define HOOK_INTERFACE_VERSION	(1)

static struct strbuf virtual_filesystem_data = STRBUF_INIT;
static struct hashmap virtual_filesystem_hashmap;
static struct hashmap parent_directory_hashmap;

struct virtualfilesystem {
	struct hashmap_entry ent; /* must be the first member! */
	const char *pattern;
	int patternlen;
};

static unsigned int(*vfshash)(const void *buf, size_t len);
static int(*vfscmp)(const char *a, const char *b, size_t len);

static int vfs_hashmap_cmp(const void *unused_cmp_data,
			   const struct hashmap_entry *he1,
			   const struct hashmap_entry *he2,
			   const void *key)
{
	const struct virtualfilesystem *vfs1 =
		container_of(he1, const struct virtualfilesystem, ent);
	const struct virtualfilesystem *vfs2 =
		container_of(he2, const struct virtualfilesystem, ent);

	return vfscmp(vfs1->pattern, vfs2->pattern, vfs1->patternlen);
}

static void get_virtual_filesystem_data(struct strbuf *vfs_data)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	int err;

	strbuf_init(vfs_data, 0);

	strvec_push(&cp.args, core_virtualfilesystem);
	strvec_pushf(&cp.args, "%d", HOOK_INTERFACE_VERSION);
	cp.use_shell = 1;
	cp.dir = get_git_work_tree();

	err = capture_command(&cp, vfs_data, 1024);
	if (err)
		die("unable to load virtual file system");
}

static int check_includes_hashmap(struct hashmap *map, const char *pattern, int patternlen)
{
	struct strbuf sb = STRBUF_INIT;
	struct virtualfilesystem vfs;
	char *slash;

	/* Check straight mapping */
	strbuf_reset(&sb);
	strbuf_add(&sb, pattern, patternlen);
	vfs.pattern = sb.buf;
	vfs.patternlen = sb.len;
	hashmap_entry_init(&vfs.ent, vfshash(vfs.pattern, vfs.patternlen));
	if (hashmap_get_entry(map, &vfs, ent, NULL)) {
		strbuf_release(&sb);
		return 1;
	}

	/*
	 * Check to see if it matches a directory or any path
	 * underneath it.  In other words, 'a/b/foo.txt' will match
	 * '/', 'a/', and 'a/b/'.
	 */
	slash = strchr(sb.buf, '/');
	while (slash) {
		vfs.pattern = sb.buf;
		vfs.patternlen = slash - sb.buf + 1;
		hashmap_entry_init(&vfs.ent, vfshash(vfs.pattern, vfs.patternlen));
		if (hashmap_get_entry(map, &vfs, ent, NULL)) {
			strbuf_release(&sb);
			return 1;
		}
		slash = strchr(slash + 1, '/');
	}

	strbuf_release(&sb);
	return 0;
}

static void includes_hashmap_add(struct hashmap *map, const char *pattern, const int patternlen)
{
	struct virtualfilesystem *vfs;

	vfs = xmalloc(sizeof(struct virtualfilesystem));
	vfs->pattern = pattern;
	vfs->patternlen = patternlen;
	hashmap_entry_init(&vfs->ent, vfshash(vfs->pattern, vfs->patternlen));
	hashmap_add(map, &vfs->ent);
}

static void initialize_includes_hashmap(struct hashmap *map, struct strbuf *vfs_data)
{
	char *buf, *entry;
	size_t len;
	int i;

	/*
	 * Build a hashmap of the virtual file system data we can use to look
	 * for cache entry matches quickly
	 */
	vfshash = ignore_case ? memihash : memhash;
	vfscmp = ignore_case ? strncasecmp : strncmp;
	hashmap_init(map, vfs_hashmap_cmp, NULL, 0);

	entry = buf = vfs_data->buf;
	len = vfs_data->len;
	for (i = 0; i < len; i++) {
		if (buf[i] == '\0') {
			includes_hashmap_add(map, entry, buf + i - entry);
			entry = buf + i + 1;
		}
	}
}

/*
 * Return 1 if the requested item is found in the virtual file system,
 * 0 for not found and -1 for undecided.
 */
int is_included_in_virtualfilesystem(const char *pathname, int pathlen)
{
	if (!core_virtualfilesystem)
		return -1;

	if (!virtual_filesystem_hashmap.tablesize && virtual_filesystem_data.len)
		initialize_includes_hashmap(&virtual_filesystem_hashmap, &virtual_filesystem_data);
	if (!virtual_filesystem_hashmap.tablesize)
		return -1;

	return check_includes_hashmap(&virtual_filesystem_hashmap, pathname, pathlen);
}

static void parent_directory_hashmap_add(struct hashmap *map, const char *pattern, const int patternlen)
{
	char *slash;
	struct virtualfilesystem *vfs;

	/*
	 * Add any directories leading up to the file as the excludes logic
	 * needs to match directories leading up to the files as well. Detect
	 * and prevent unnecessary duplicate entries which will be common.
	 */
	if (patternlen > 1) {
		slash = strchr(pattern + 1, '/');
		while (slash) {
			vfs = xmalloc(sizeof(struct virtualfilesystem));
			vfs->pattern = pattern;
			vfs->patternlen = slash - pattern + 1;
			hashmap_entry_init(&vfs->ent, vfshash(vfs->pattern, vfs->patternlen));
			if (hashmap_get_entry(map, vfs, ent, NULL))
				free(vfs);
			else
				hashmap_add(map, &vfs->ent);
			slash = strchr(slash + 1, '/');
		}
	}
}

static void initialize_parent_directory_hashmap(struct hashmap *map, struct strbuf *vfs_data)
{
	char *buf, *entry;
	size_t len;
	int i;

	/*
	 * Build a hashmap of the parent directories contained in the virtual
	 * file system data we can use to look for matches quickly
	 */
	vfshash = ignore_case ? memihash : memhash;
	vfscmp = ignore_case ? strncasecmp : strncmp;
	hashmap_init(map, vfs_hashmap_cmp, NULL, 0);

	entry = buf = vfs_data->buf;
	len = vfs_data->len;
	for (i = 0; i < len; i++) {
		if (buf[i] == '\0') {
			parent_directory_hashmap_add(map, entry, buf + i - entry);
			entry = buf + i + 1;
		}
	}
}

static int check_directory_hashmap(struct hashmap *map, const char *pathname, int pathlen)
{
	struct strbuf sb = STRBUF_INIT;
	struct virtualfilesystem vfs;

	/* Check for directory */
	strbuf_reset(&sb);
	strbuf_add(&sb, pathname, pathlen);
	strbuf_addch(&sb, '/');
	vfs.pattern = sb.buf;
	vfs.patternlen = sb.len;
	hashmap_entry_init(&vfs.ent, vfshash(vfs.pattern, vfs.patternlen));
	if (hashmap_get_entry(map, &vfs, ent, NULL)) {
		strbuf_release(&sb);
		return 0;
	}

	strbuf_release(&sb);
	return 1;
}

/*
 * Return 1 for exclude, 0 for include and -1 for undecided.
 */
int is_excluded_from_virtualfilesystem(const char *pathname, int pathlen, int dtype)
{
	if (!core_virtualfilesystem)
		return -1;

	if (dtype != DT_REG && dtype != DT_DIR && dtype != DT_LNK)
		die(_("is_excluded_from_virtualfilesystem passed unhandled dtype"));

	if (dtype == DT_REG || dtype == DT_LNK) {
		int ret = is_included_in_virtualfilesystem(pathname, pathlen);
		if (ret > 0)
			return 0;
		if (ret == 0)
			return 1;
		return ret;
	}

	if (dtype == DT_DIR) {
		int ret = is_included_in_virtualfilesystem(pathname, pathlen);
		if (ret > 0)
			return 0;

		if (!parent_directory_hashmap.tablesize && virtual_filesystem_data.len)
			initialize_parent_directory_hashmap(&parent_directory_hashmap, &virtual_filesystem_data);
		if (!parent_directory_hashmap.tablesize)
			return -1;

		return check_directory_hashmap(&parent_directory_hashmap, pathname, pathlen);
	}

	return -1;
}

struct apply_virtual_filesystem_stats {
	int nr_unknown;
	int nr_vfs_dirs;
	int nr_vfs_rows;
	int nr_bulk_skip;
	int nr_explicit_skip;
};

static void clear_ce_flags_virtualfilesystem_1(struct index_state *istate, int select_mask, int clear_mask,
					       struct apply_virtual_filesystem_stats *stats)
{
	char *buf, *entry;
	int i;

	if (!virtual_filesystem_data.len)
		get_virtual_filesystem_data(&virtual_filesystem_data);

	/* clear specified flag bits for everything in the virtual file system */
	entry = buf = virtual_filesystem_data.buf;
	for (i = 0; i < virtual_filesystem_data.len; i++) {
		if (buf[i] == '\0') {
			struct cache_entry *ce;
			int pos, len;

			stats->nr_vfs_rows++;

			len = buf + i - entry;

			/* look for a directory wild card (ie "dir1/") */
			if (buf[i - 1] == '/') {
				stats->nr_vfs_dirs++;
				if (ignore_case)
					adjust_dirname_case(istate, entry);
				pos = index_name_pos(istate, entry, len);
				if (pos < 0) {
					for (pos = -pos - 1; pos < istate->cache_nr; pos++) {
						ce = istate->cache[pos];
						if (fspathncmp(ce->name, entry, len))
							break;

						if (select_mask && !(ce->ce_flags & select_mask))
							continue;

						if (ce->ce_flags & clear_mask)
							stats->nr_bulk_skip++;
						ce->ce_flags &= ~clear_mask;
					}
				}
			} else {
				if (ignore_case) {
					ce = index_file_exists(istate, entry, len, ignore_case);
				} else {
					int pos = index_name_pos(istate, entry, len);

					ce = NULL;
					if (pos >= 0)
						ce = istate->cache[pos];
				}

				if (ce) {
					do {
						if (!select_mask || (ce->ce_flags & select_mask)) {
							if (ce->ce_flags & clear_mask)
								stats->nr_explicit_skip++;
							ce->ce_flags &= ~clear_mask;
						}

						/*
						 * There may be aliases with different cases of the same
						 * name that also need to be modified.
						 */
						if (ignore_case)
							ce = index_file_next_match(istate, ce, ignore_case);
						else
							break;

					} while (ce);
				} else {
					stats->nr_unknown++;
				}
			}

			entry += len + 1;
		}
	}
}

/*
 * Clear the specified flags for all entries in the virtual file system
 * that match the specified select mask. Returns the number of entries
 * processed.
 */
int clear_ce_flags_virtualfilesystem(struct index_state *istate, int select_mask, int clear_mask)
{
	struct apply_virtual_filesystem_stats stats = {0};

	clear_ce_flags_virtualfilesystem_1(istate, select_mask, clear_mask, &stats);
	return istate->cache_nr;
}

/*
 * Update the CE_SKIP_WORKTREE bits based on the virtual file system.
 */
void apply_virtualfilesystem(struct index_state *istate)
{
	int i;
	struct apply_virtual_filesystem_stats stats = {0};

	if (!git_config_get_virtualfilesystem())
		return;

	trace2_region_enter("vfs", "apply", the_repository);

	/* set CE_SKIP_WORKTREE bit on all entries */
	for (i = 0; i < istate->cache_nr; i++)
		istate->cache[i]->ce_flags |= CE_SKIP_WORKTREE;

	clear_ce_flags_virtualfilesystem_1(istate, 0, CE_SKIP_WORKTREE, &stats);
	if (stats.nr_vfs_rows > 0) {
		trace2_data_intmax("vfs", the_repository, "apply/tracked", stats.nr_bulk_skip + stats.nr_explicit_skip);

		trace2_data_intmax("vfs", the_repository, "apply/vfs_rows", stats.nr_vfs_rows);
		trace2_data_intmax("vfs", the_repository, "apply/vfs_dirs", stats.nr_vfs_dirs);

		trace2_data_intmax("vfs", the_repository, "apply/nr_unknown", stats.nr_unknown);
		trace2_data_intmax("vfs", the_repository, "apply/nr_bulk_skip", stats.nr_bulk_skip);
		trace2_data_intmax("vfs", the_repository, "apply/nr_explicit_skip", stats.nr_explicit_skip);
	}

	trace2_region_leave("vfs", "apply", the_repository);
}

/*
 * Free the virtual file system data structures.
 */
void free_virtualfilesystem(void) {
	hashmap_clear_and_free(&virtual_filesystem_hashmap, struct virtualfilesystem, ent);
	hashmap_clear_and_free(&parent_directory_hashmap, struct virtualfilesystem, ent);
	strbuf_release(&virtual_filesystem_data);
}
