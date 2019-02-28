#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

#define NCPU 4
#define CREATE_N_OBJECTS 100

void *xmalloc(size_t sz) {
    void *mm = malloc(sz);
    if (!mm)
        die("malloc()");
    return mm;
}
void *xrealloc(void *m, size_t sz) {
    void *mm = realloc(m, sz);
    if (!mm)
        die("realloc()");
    return mm;
}
char *xstrdup(const char *s) {
    size_t len = strlen(s);
    char *new_s = xmalloc(len+1);
    new_s[len] = '\0';
    memcpy(new_s, s, len);
    return new_s;
}



char *get_invk_name(const char *script_name) {
    const char *extension = strstr(script_name, ".lua");
    if (!extension)
        die("invalid script name: %s, expected *.lua", script_name);
    char *adjusted = xmalloc(extension - script_name + strlen("_update") + 1);
    strncpy(adjusted, script_name, extension - script_name);
    strcpy(adjusted + (extension - script_name), "_update");
    return adjusted;
}

struct script_description {
    char *script_name;
    char *invoke_name;
};

struct script_object { 
    int script_id; //shared script description
    long data_table_idx; //instance specific data
};

struct lua_state_bundle { 
    lua_State *L;    
    struct script_description *loaded_scripts;
    pthread_mutex_t script_list_mtx;
    struct script_object *objects;
    long obj_cap;
    long obj_len;
    long worker_id;
} lstates[NCPU];


pthread_mutex_t print_lock = PTHREAD_MUTEX_INITIALIZER;
int lua_stop = 0; //can be called from a script to stop updating

int locked_print_l(lua_State *L) {
    pthread_mutex_lock(&print_lock);
    const char *to_print = "error, expecting string";
    if (lua_isstring(L, 1))
        to_print = lua_tostring(L, 1);
    fprintf(stdout, "%s\n", to_print);
    pthread_mutex_unlock(&print_lock);
    return 0;
}
int lua_stop_l(lua_State *L) {
    lua_stop = 1;
    return 0;
}
void init_lstate_cfunctions(struct lua_state_bundle *bundle) {
    lua_pushcfunction(bundle->L, locked_print_l);
    lua_setglobal(bundle->L, "locked_print");
    lua_pushcfunction(bundle->L, lua_stop_l);
    lua_setglobal(bundle->L, "lua_stop");
}


//returns the script descriptor, makes sure the script is loaded
struct script_description *ensure_loaded_script(struct lua_state_bundle *bundle, const char *script_name) {
    assert(script_name);
    pthread_mutex_lock(&bundle->script_list_mtx);
    //see if the script has been loaded before for this lua_state
    bool do_load = true;

    //should probably use a hashtable
    int nloaded;
    //loop looking for the script name or a sentinel
    for (nloaded=0; bundle->loaded_scripts[nloaded].script_name; nloaded++) {
        if (strcmp(bundle->loaded_scripts[nloaded].script_name, script_name) == 0) {
            do_load = false;
            break;
        }
    }

    if (do_load) {
        if (luaL_loadfile(bundle->L, script_name) != 0) {
            die("failed to load script %s", script_name);
        }
        //call it once to initialize stuff
        if (lua_pcall(bundle->L, 0, 0, 0) != 0) {
            die("a lua error occured while initializing script %s\n%s", script_name, lua_tostring(bundle->L, -1));
        }
        //add it to the list
        assert(!bundle->loaded_scripts[nloaded].script_name);
        bundle->loaded_scripts = xrealloc(bundle->loaded_scripts, sizeof(struct script_description) * (nloaded + 2));
        bundle->loaded_scripts[nloaded].script_name = xstrdup(script_name);
        bundle->loaded_scripts[nloaded].invoke_name = get_invk_name(script_name);
        bundle->loaded_scripts[nloaded+1].script_name = NULL; //set sentinel
        bundle->loaded_scripts[nloaded+1].invoke_name = NULL; 
    }
    pthread_mutex_unlock(&bundle->script_list_mtx);
    assert(strcmp(bundle->loaded_scripts[nloaded].script_name, script_name) == 0);
    return bundle->loaded_scripts + nloaded;
}

void *lua_worker(void *data) {
    struct lua_state_bundle *bundle = data;
    assert(bundle && bundle->L && bundle->loaded_scripts);
    while (!lua_stop) {
        for (int i=0; i<bundle->obj_len; i++) {
            struct script_description *desc = &bundle->loaded_scripts[bundle->objects[i].script_id];
            assert(desc && desc->script_name && desc->invoke_name);
            lua_getglobal(bundle->L, desc->invoke_name);
            lua_rawgeti(bundle->L, LUA_REGISTRYINDEX, bundle->objects[i].data_table_idx);
            //              L       ARGS RESULTS IDK
            if (lua_pcall(bundle->L, 1,    0,     0) != 0) {
                die("an error occured while calling %s, %s", desc->invoke_name, lua_tostring(bundle->L, -1));
            }
        }
    }
    return NULL;
}

void init_states(void) {
    for (int i=0; i<NCPU; i++) {
        lua_State *L = luaL_newstate();    
        if (!L)
            die("couldnt start lua");
        luaL_openlibs(L);
        memset(lstates + i, 0, sizeof(struct lua_state_bundle));
        lstates[i].L = L;
        //set sentinel value, there are currently no loaded scripts
        lstates[i].loaded_scripts = xmalloc(sizeof(struct script_description));
        lstates[i].loaded_scripts[0].script_name = NULL;
        lstates[i].loaded_scripts[0].invoke_name = NULL; 
        if (pthread_mutex_init(&lstates[i].script_list_mtx, NULL) != 0)
            die("failed to initialize mutex");
        lstates[i].worker_id = i;
        
        init_lstate_cfunctions(lstates+i);
    }
}

void destroy_states() {
    for (int i=0; i<NCPU; i++) {
        for (int j=0; lstates[i].loaded_scripts[j].script_name; j++) {
            free(lstates[i].loaded_scripts[j].script_name);
            free(lstates[i].loaded_scripts[j].invoke_name);
        }
        free(lstates[i].loaded_scripts);
        free(lstates[i].objects);
        lua_close(lstates[i].L);
        lstates[i].L = NULL;
    }
}

//we're creating an empty table and storing it in a unique integer index in the registry
long make_lua_registry_table(struct lua_state_bundle *bundle) {
    lua_newtable(bundle->L);
    //store some info for the lua script
    lua_pushstring(bundle->L, "worker_id");
    lua_pushinteger(bundle->L, bundle->worker_id);
    lua_settable(bundle->L, -3);
    lua_pushstring(bundle->L, "instance_id");
    lua_pushinteger(bundle->L, bundle->obj_len);
    lua_settable(bundle->L, -3);
    long reg_idx = luaL_ref(bundle->L, LUA_REGISTRYINDEX);
    return reg_idx;
}

void init_objects(struct lua_state_bundle *bundle, int nobjects) {
    //files that are supposed to exist on the file system
    const char *scripts[] = { "script_a.lua", "script_b.lua" }; 
    int nscripts = sizeof scripts / sizeof scripts[0];
    for (int i=0; i<nobjects; i++) {
        int idx = rand() % nscripts;
        idx = idx > 0 ? idx : -idx;
        struct script_description *desc = ensure_loaded_script(bundle, scripts[idx]);
        assert(desc && desc->script_name && desc->invoke_name);
        if (!bundle->objects || bundle->obj_len == bundle->obj_cap) {
            if (!bundle->objects) {
                bundle->obj_cap = 1;
                bundle->obj_len = 0;
                bundle->objects = xmalloc(sizeof(struct script_object) * bundle->obj_cap);
            }
            else {
                bundle->obj_cap *= 2;
                bundle->objects = xrealloc(bundle->objects, sizeof(struct script_object) * bundle->obj_cap);
            }
        }
        assert(bundle->obj_len < bundle->obj_cap);
        bundle->objects[i].script_id = desc - bundle->loaded_scripts;
        bundle->objects[i].data_table_idx = make_lua_registry_table(bundle);
        bundle->obj_len++;
    }
}

int main(void) {
    pthread_t threads[NCPU];
    init_states();
    srand(0xFFACADE0);
    for (int i=0; i<NCPU; i++) {
        init_objects(lstates + i, CREATE_N_OBJECTS / NCPU);
    }
    for (int i=0; i<NCPU; i++) {
        if (pthread_create(threads+i, NULL, lua_worker, lstates+i /*args*/) != 0)
            die("pthread creation failed\n");
    }
    for (int i=0; i<NCPU; i++) {
        if (pthread_join(threads[i], NULL) != 0)
            die("pthread join failed\n");
    }
    destroy_states();
}

