



typedef struct {
  unsigned char ch;
  unsigned char count;
} tre_filter_profile_t;

typedef struct {
  /* Length of the window where the character counts are kept. */
  int window_len;
  /* Required character counts table. */
  tre_filter_profile_t *profile;
} tre_filter_t;


int
tre_filter_find(const unsigned char *str, size_t len, tre_filter_t *filter);
