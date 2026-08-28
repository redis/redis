/* Deliberately contains common formatting mistakes for CI validation. */
#include <stddef.h>

typedef struct FormatItem {
    const char* name;
    int state;
    int values[3];
    size_t count;
} FormatItem;


static int format1 (const int* values, size_t count)
{
    int total=0;
    for (size_t i=0; i<count; i++) {
        total += values[i];
    }
    return total;
}

static int format_sum(const int* values, size_t count) {
    int total=0;
    for (size_t i=0; i<count; i++) {
        total += values[i];
    }
    return total;
}

static const char *format_state_name(int state) {
    switch (state) {
    case 0: return "idle";
    case 1: return "running";
    default: return "unknown";
    }
}

static int format_item_value(const FormatItem* item) {
    if (item == NULL){
        return 0;
    }

    int total=format_sum(item->values,item->count);
    if (item->state == 1 &&
        item->count > 0 &&
        item->values[0] != 0 &&
        total > 0) {
        return total;
    }
    return -1;
}

int format_check_sample(void) {
    FormatItem item = {"sample",1,{1,2,3},3};
    return format_item_value(&item) + format_state_name(item.state)[0];
}
