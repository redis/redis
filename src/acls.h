#ifndef __ACLS_H__
#define __ACLS_H__

// <username> <password> [<acl> <acl> ... <acl>]
typedef unsigned long long acl_t;

#define CMD_ACL_INDEX(x) ((x/8)/(sizeof(acl_t)))
#define CMD_ACL_VALUE(i, x) (((acl_t)1) << (x % (sizeof(acl_t)*8)))
#define ACL_DEFAULT_USER_NAME "default"
#define ACL_DEFAULT_ARRAY_SIZE 8
#define ACL_DEFAULT_GROUP_SIZE 8

typedef struct aclGroup {
    char *name;
    acl_t *acls;
} aclGroup;

// groupFlags has users acl group information 
// and gorupOpFlags's specific bit is 1, it means add group
// 0 means removing group
typedef struct userAcl {
    char *name;
    char *passwd;
    int groupFlags;
    int groupOpFlags;
    acl_t *acls;
} userAcl;

aclGroup *createAclGroups();
userAcl *getUserAcl(char *username);

int loadAcls(aclGroup *aclGroups, const char *filename);
int increaseAclCmdSize();
int getAllAclsIndex();

void addAcl(acl_t *acls, int index, acl_t value);
void removeAcl(acl_t *acls, int index, acl_t value);
void addCommandToAclGroup(aclGroup *aclGroups, int flags, int index, acl_t value);
void reloadUserAcls();

#endif
