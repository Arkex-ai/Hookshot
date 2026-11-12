/* Authored mod/pack routing corpus; prompts are a controlled dispatch provider. */

static const char *generated, *merged;
static int saw_mod_prompt, saw_merge;
static int attribute_calls;
DWORD authored_mod_attributes(const char *path) {
    ++attribute_calls;
    return GetFileAttributesA(path);
}
static void count_mod_entry(void *out, const char *name, int directory, uint64_t size) {
    (void)name; (void)directory; (void)size;
    ++*(int *)out;
}
int hos_prompts_romfs(const char *key,const char *mod,char *out,size_t cap) {
    if (!strcmp(key,"ui/prompt") && mod) saw_mod_prompt++;
    if (!strcmp(key,"ui/fixture.rsizetable.zs") && mod) {
        saw_merge++; snprintf(out,cap,"%s",merged); return 1;
    }
    if (!strcmp(key,"ui/generated") && !mod) { snprintf(out,cap,"%s",generated); return 1; }
    return 0;
}
void hos_mods_init(const char *image);
static int contents(const char *name,const char *expected) {
    uint64_t result=0,h=open_guest(name,1,&result);
    if(result) return 0;
    uint64_t n=read_guest(h,0,0x2000,128);
    int ok=n==strlen(expected)&&!memcmp(GP(0x2000),expected,(size_t)n);
    close_guest(h);return ok;
}
int main(int argc,char **argv) {
    if(argc!=8)return 90;
    set_env("HOS_FS_DELAY_MS",NULL);set_env("HOS_FS_WATCH",NULL);set_env("HOS_FS_TRACE",NULL);
    hos_romfs_dir=argv[2];generated=argv[4];merged=argv[5];wide_file=argv[6];
    set_env("HOS_MODS",argv[3]);set_env("HOS_FS_OVERLAY",NULL);set_env("HOS_NO_MOVIES",NULL);
    CHECK(!hos_fs_packed());
    CHECK(mount("rom",hos_romfs_dir,0));CHECK(hos_fs_pack_open(argv[1]));CHECK(hos_fs_packed());
    hos_mods_init("authored-image");
    int extras = 0;
    attribute_calls = 0;
    CHECK(hos_mods_dir_extras("", NULL, count_mod_entry, &extras) == extras && extras > 0);
    CHECK(attribute_calls == 0);   /* NULL base must not issue any host filesystem probes. */
    extras = 0;
    CHECK(hos_mods_dir_extras("", hos_romfs_dir, count_mod_entry, &extras) == extras && extras > 0);
    CHECK(attribute_calls > 0);   /* A real loose base still filters its existing entries. */
    CHECK(contents("rom:/pattern","LAST"));
    CHECK(contents("rom:/base-only","BASEONLY"));
    CHECK(contents("rom:/nested-only","NESTED"));
    CHECK(contents("rom:/new-only","ADDED"));
    CHECK(contents("rom:/new-dir/new","NEW"));
    CHECK(contents("rom:/UI/generated","PROMPT"));
    CHECK(contents("rom:/UI/prompt","MODPROMPT")&&saw_mod_prompt);
    CHECK(contents("rom:/UI/fixture.rsizetable.zs","MERGED")&&saw_merge);
    uint64_t length=0;char bytes[4];
    CHECK(hos_fs_romfs_read("pattern",0,bytes,4,&length)&&!memcmp(bytes,"BASE",4));
    use_wide=1;CHECK(contents("rom:/pattern","WIDE"));
    set_env("HOS_FS_OVERLAY",argv[7]);CHECK(contents("rom:/pattern","OVERLAY"));
    set_env("HOS_FS_OVERLAY",NULL);use_wide=0;
    uint64_t result=0,h=open_directory("rom:/",3,&result);
    CHECK(result==0&&count_directory(h)==6);
    CHECK(read_directory(h,0x2000,8,&result)==6&&result==0);
    int pattern=0,added=0,nested=0,newdir=0,stale=0;
    for(unsigned i=0;i<6;i++) {
        const char *name=(const char *)GP(0x2000+0x310*i);
        pattern+=!strcmp(name,"pattern");added+=!strcmp(name,"new-only");
        nested+=!strcmp(name,"nested-only");newdir+=!strcmp(name,"new-dir");stale+=!strcmp(name,"stale");
    }
    CHECK(pattern==1&&added==1&&nested==1&&newdir==1&&!stale);
    close_directory(h);
    h=open_directory("rom:/new-dir",3,&result);
    CHECK(result==0&&count_directory(h)==1);
    CHECK(read_directory(h,0x2000,1,&result)==1&&!strcmp((char *)GP(0x2000),"new"));close_directory(h);
    printf("%u actual-loader corpus assertions passed\n",checks);return 0;
}
