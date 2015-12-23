#ifndef __ACLS_H__
#define __ACLS_H__

// <username> <password> [<acl> <acl> ... <acl>]
typedef unsigned long acl_t;

#define CMD_ACL_VALUE(x) (((acl_t)1) << x)
#define ACL_ARRAY_NUM 8
#define ACL_DEFAULT_USER_NAME "default"

typedef struct aclGroup {
    char *name;
    acl_t acls[ACL_ARRAY_NUM];
} aclGroup;

typedef struct userAcl {
    char *name;
    char *passwd;
    acl_t acls[ACL_ARRAY_NUM];
} userAcl;

extern aclGroup aclGroups[];

int loadAcls(const char *filename);
userAcl *getUserAcl(char *username);
void initAcls(acl_t acls[ACL_ARRAY_NUM]);
void setAcls(acl_t src[ACL_ARRAY_NUM], acl_t tar[ACL_ARRAY_NUM]);

#endif
