#include "lib9.h"
#include "logfs.h"
#include "local.h"

enum {
	GROUPMOD = 127
};

static int
groupcompare(Group *g, char *uid)
{
	return g->uid == uid;
}

static int
unamecompare(Uname *u, char *uname)
{
	return u->uname == uname;
}

static int
groupallocsize(void *key)
{
	USED(key);
	return sizeof(Group);
}

static int
unameallocsize(void *key)
{
	USED(key);
	return sizeof(Uname);
}

char *
logfsgroupmapnew(GroupMap **groupmapp, UnameMap **unamemapp)
{
	char *errmsg;
	errmsg = logfsmapnew(GROUPMOD, logfshashulong, (int (*)(void *, void *))groupcompare,
		groupallocsize, nil, groupmapp);
	if(errmsg)
		return errmsg;
	errmsg = logfsmapnew(GROUPMOD, logfshashulong, (int (*)(void *, void *))unamecompare,
		unameallocsize, nil, unamemapp);
	if(errmsg)
		logfsmapfree(groupmapp);
	return errmsg;
}

char *
logfsgroupmapnewentry(GroupMap *gm, UnameMap *um, char *uid, char *uname, Group **groupp, Uname **unamep)
{
	char *errmsg;
	errmsg = logfsmapnewentry(gm, uid, groupp);
	if(errmsg)
		return errmsg;
	errmsg = logfsgroupsetnew(&(*groupp)->members);
	if(errmsg) {
		logfsmapdeleteentry(gm, uid);
		return errmsg;
	}
	errmsg = logfsmapnewentry(um, uname, unamep);
	if(errmsg) {
		logfsgroupsetfree(&(*groupp)->members);
		logfsmapdeleteentry(gm, uid);
		return errmsg;
	}
	(*groupp)->uid = uid;
	(*groupp)->uname = uname;
	(*unamep)->uname = uname;
	(*unamep)->g = *groupp;
	return nil;
}

char *
logfsgroupmapfinduname(GroupMap *m, char *uid)
{
	Group *g;
	g = logfsgroupmapfindentry(m, uid);
	if(g)
		return g->uname;
	return nil;
}

char *
logfsunamemapfinduid(UnameMap *m, char *uname)
{
	Uname *u;
	u = logfsunamemapfindentry(m, uname);
	if(u)
		return u->g->uid;
	return nil;
}
