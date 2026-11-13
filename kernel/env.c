/* Global environment variables (PATH, HOME, ...). */
#include "kernel.h"

#define ENV_MAX 32
static struct { char name[VX_NAME_MAX]; char value[VX_PATH_MAX]; } vars[ENV_MAX];

static int find(const char *name)
{
    for (int i = 0; i < ENV_MAX; i++)
        if (vars[i].name[0] && strcmp(vars[i].name, name) == 0) return i;
    return -1;
}

const char *env_get(const char *name)
{
    int i = find(name);
    return i < 0 ? NULL : vars[i].value;
}

int env_set(const char *name, const char *value)
{
    int i = find(name);
    if (!name[0] || strlen(name) >= VX_NAME_MAX || strlen(value) >= VX_PATH_MAX) return -1;
    for (int k = 0; i < 0 && k < ENV_MAX; k++)
        if (!vars[k].name[0]) i = k;
    if (i < 0) return -1;
    strcpy(vars[i].name, name);
    strcpy(vars[i].value, value);
    return 0;
}

void env_unset(const char *name)
{
    int i = find(name);
    if (i >= 0) vars[i].name[0] = 0;
}

/* Iterate: returns 0 past the end; *name is NULL for empty slots. */
int env_at(int index, const char **name, const char **value)
{
    if (index < 0 || index >= ENV_MAX) return 0;
    *name = vars[index].name[0] ? vars[index].name : NULL;
    *value = vars[index].value;
    return 1;
}
