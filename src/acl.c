            "Error loading ACLs, opening file '%s': %s",
            filename, strerror(errno));
        return errors;
    }
    /* Load the whole file as a single string in memory. */
    sds acls = sdsempty();
    while(fgets(buf,sizeof(buf),fp) != NULL)
        acls = sdscat(acls,buf);
    fclose(fp);
    /* Split the file into lines and attempt to load each line. */
    int totlines;
    sds *lines, errors = sdsempty();
    lines = sdssplitlen(acls,strlen(acls),"\n",1,&totlines);
    sdsfree(acls);
    /* We do all the loading in a fresh instance of the Users radix tree,
     * so if there are errors loading the ACL file we can rollback to the
     * old version. */
    rax *old_users = Users;
    Users = raxNew();
    /* Load each line of the file. */
    for (int i = 0; i < totlines; i++) {
        sds *argv;
        int argc;
        int linenum = i+1;
        lines[i] = sdstrim(lines[i]," \t\r\n");
        /* Skip blank lines */
        if (lines[i][0] == '\0') continue;
        /* Skip comment lines */
        if (lines[i][0] == '#') continue;
        /* Split into arguments */
        argv = sdssplitlen(lines[i],sdslen(lines[i])," ",1,&argc);
        if (argv == NULL) {
            errors = sdscatprintf(errors,
                     "%s:%d: unbalanced quotes in acl line. ",
                     server.acl_filename, linenum);
            continue;
        }
        /* Skip this line if the resulting command vector is empty. */
        if (argc == 0) {
            sdsfreesplitres(argv,argc);
            continue;
        }
        /* The line should start with the \"user\" keyword. */
        if (strcmp(argv[0],"user") || argc < 2) {
            errors = sdscatprintf(errors,
                     "%s:%d should start with user keyword followed "
                     "by the username. ", server.acl_filename,
                     linenum);
            sdsfreesplitres(argv,argc);
            continue;
        }
        /* Spaces are not allowed in usernames. */
        if (ACLStringHasSpaces(argv[1],sdslen(argv[1]))) {
            errors = sdscatprintf(errors,
                     "'%s:%d: username '%s' contains invalid characters. ",
                     server.acl_filename, linenum, argv[1]);
            sdsfreesplitres(argv,argc);
            continue;
        }
        user *u = ACLCreateUser(argv[1],sdslen(argv[1]));
        /* If the user already exists we assume it's an error and abort. */
        if (!u) {
            errors = sdscatprintf(errors,"WARNING: Duplicate user '%s' found on line %d. ", argv[1], linenum);
            sdsfreesplitres(argv,argc);
            continue;
        }
        /* Finally process the options and validate they can
         * be cleanly applied to the user. If any option fails
         * to apply, the other values won't be applied since
         * all the pending changes will get dropped. */
        int merged_argc;
        sds *acl_args = ACLMergeSelectorArguments(argv + 2, argc - 2, &merged_argc, NULL);
        if (!acl_args) {
            errors = sdscatprintf(errors,
