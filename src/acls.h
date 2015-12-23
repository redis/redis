#ifndef __ACLS_H__
#define __ACLS_H__

// <username> <password> [<acl> <acl> ... <acl>]
#define CMD_ACL_VALUE(x) (((unsigned long long)1) << x)
#define ACL_ARRAY_NUM 4

typedef struct aclGroup {
    char *name;
    unsigned long long acls[ACL_ARRAY_NUM];
} aclGroup;

typedef struct userAcl {
    char *name;
    char *passwd;
    unsigned long long acls[ACL_ARRAY_NUM];
} userAcl;

extern aclGroup aclGroups[];

int loadAcls(const char *filename);
void initAcls(unsigned long long acls[ACL_ARRAY_NUM]);
void setAcls(unsigned long long src[ACL_ARRAY_NUM], unsigned long long tar[ACL_ARRAY_NUM]);

#endif
