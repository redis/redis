unsigned int CustomkeyHashSlot(char *key, int keylen) {
	/* Add the custom hash function here */
    unsigned int hash = 0;
    for(int i = 0; i < keylen; i++) {
        char c = key[i];
        int a = c - '0';
        hash = (hash * 10) + a;     
    } 

    return hash % 0x3FFF;
	// return 0xFFFF;
}