#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>

#include <3ds.h>

#include "blz.h"

Handle save_session;
FS_Archive save_archive;

// http://3dbrew.org/wiki/Nandrw/sys/SecureInfo_A
const char regions[7][4] = {
    "JPN",
    "USA",
    "EUR",
    "EUR",
    "CHN",
    "KOR",
    "TWN"
};

typedef enum
{
    STATE_NONE,
    STATE_INITIALIZE,
    STATE_INITIAL,
    STATE_SELECT_VERSION,
    STATE_SELECT_SLOT,
    STATE_SELECT_FIRMWARE,
    STATE_DOWNLOAD_PAYLOAD,
    STATE_COMPRESS_PAYLOAD,
    STATE_INSTALL_PAYLOAD,
    STATE_INSTALLED_PAYLOAD,
    STATE_ERROR,
} state_t;

char status[256];

struct {
    bool enabled;
    size_t offset;
    char path[256];
} payload_embed;

Result get_redirect(char *url, char *out, size_t out_size, char *user_agent)
{
    Result ret;

    httpcContext context;
    ret = httpcOpenContext(&context, HTTPC_METHOD_GET, url, 0);
    if(R_FAILED(ret)) return ret;

    ret = httpcAddRequestHeaderField(&context, "User-Agent", user_agent);
    if(R_SUCCEEDED(ret)) ret = httpcBeginRequest(&context);

    if(R_FAILED(ret))
    {
        httpcCloseContext(&context);
        return ret;
    }

    ret = httpcGetResponseHeader(&context, "Location", out, out_size);
    httpcCloseContext(&context);

    return ret;
}

Result download_file(httpcContext *context, void** buffer, size_t* size, char* user_agent)
{
    Result ret;

    ret = httpcAddRequestHeaderField(context, "User-Agent", user_agent);
    if(R_FAILED(ret)) return ret;

    ret = httpcBeginRequest(context);
    if(R_FAILED(ret)) return ret;

    u32 status_code = 0;
    ret = httpcGetResponseStatusCode(context, &status_code);
    if(R_FAILED(ret)) return ret;

    if(status_code != 200) return -1;

    u32 sz = 0;
    ret = httpcGetDownloadSizeState(context, NULL, &sz);
    if(R_FAILED(ret)) return ret;

    void* buf = malloc(sz);
    if(!buf) return -2;

    memset(buf, 0, sz);

    ret = httpcDownloadData(context, buf, sz, NULL);
    if(R_FAILED(ret))
    {
        free(buf);
        return ret;
    }

    if(size) *size = sz;
    if(buffer) *buffer = buf;
    else free(buf);

    return 0;
}

Result read_savedata(const char* path, void** data, size_t* size)
{
    if(!path || !data || !size) return -1;

    Result ret = -1;
    int fail = 0;
    void* buffer = NULL;

    fsUseSession(save_session);
    ret = FSUSER_OpenArchive(&save_archive, ARCHIVE_SAVEDATA, (FS_Path){PATH_EMPTY, 1, (u8*)""});
    if(R_FAILED(ret))
    {
        fail = -1;
        goto readFail;
    }

    Handle file = 0;
    ret = FSUSER_OpenFile(&file, save_archive, fsMakePath(PATH_ASCII, path), FS_OPEN_READ, 0);
    if(R_FAILED(ret))
    {
        fail = -2;
        goto readFail;
    }

    u64 file_size = 0;
    ret = FSFILE_GetSize(file, &file_size);

    buffer = malloc(file_size);
    if(!buffer)
    {
        fail = -3;
        goto readFail;
    }

    u32 bytes_read = 0;
    ret = FSFILE_Read(file, &bytes_read, 0, buffer, file_size);
    if(R_FAILED(ret))
    {
        fail = -4;
        goto readFail;
    }

    ret = FSFILE_Close(file);
    if(R_FAILED(ret))
    {
        fail = -5;
        goto readFail;
    }

readFail:
    FSUSER_CloseArchive(save_archive);
    fsEndUseSession();
    if(fail)
    {
        sprintf(status, "Failed to read file: %d\n     %08lX %08lX", fail, ret, bytes_read);
        if(buffer) free(buffer);
    }
    else
    {
        sprintf(status, "Successfully read file.\n     %08lX               ", bytes_read);
        *data = buffer;
        *size = bytes_read;
    }

    return ret;
}

Result write_savedata(const char* path, const void* data, size_t size)
{
    if(!path || !data || size == 0) return -1;

    Result ret = -1;
    int fail = 0;

    fsUseSession(save_session);
    ret = FSUSER_OpenArchive(&save_archive, ARCHIVE_SAVEDATA, (FS_Path){PATH_EMPTY, 1, (u8*)""});
    if(R_FAILED(ret))
    {
        fail = -1;
        goto writeFail;
    }

    // delete file
    FSUSER_DeleteFile(save_archive, fsMakePath(PATH_ASCII, path));
    FSUSER_ControlArchive(save_archive, ARCHIVE_ACTION_COMMIT_SAVE_DATA, NULL, 0, NULL, 0);

    Handle file = 0;
    ret = FSUSER_OpenFile(&file, save_archive, fsMakePath(PATH_ASCII, path), FS_OPEN_CREATE | FS_OPEN_WRITE, 0);
    if(R_FAILED(ret))
    {
        fail = -2;
        goto writeFail;
    }

    u32 bytes_written = 0;
    ret = FSFILE_Write(file, &bytes_written, 0, data, size, FS_WRITE_FLUSH | FS_WRITE_UPDATE_TIME);
    if(R_FAILED(ret))
    {
        fail = -3;
        goto writeFail;
    }

    ret = FSFILE_Close(file);
    if(R_FAILED(ret))
    {
        fail = -4;
        goto writeFail;
    }

    ret = FSUSER_ControlArchive(save_archive, ARCHIVE_ACTION_COMMIT_SAVE_DATA, NULL, 0, NULL, 0);
    if(R_FAILED(ret)) fail = -5;

writeFail:
    FSUSER_CloseArchive(save_archive);
    fsEndUseSession();
    if(fail) sprintf(status, "Failed to write to file: %d\n     %08lX %08lX", fail, ret, bytes_written);
    else sprintf(status, "Successfully wrote to file!\n     %08lX               ", bytes_written);

    return ret;
}


void remove_newline(char *line)
{
    int len = strlen(line);
    if(len == 0)return;

    if(line[len - 1] == '\n')
    {
        line[len - 1] = 0;
        if(len > 1)
        {
            if(line[len - 2] == '\r')
            {
                line[len - 2] = 0;
            }
        }
    }
}

//Format of the config file: each line is for a different exploit. Each parameter is seperated by spaces(' '). "<exploitname> <titlename> <flags_bitmask> <list_of_programIDs>"
Result load_exploitlist_config(char *filepath, u64 *cur_programid, char *out_exploitname, char *out_titlename, u32* out_flags_bitmask)
{
    FILE *f;
    int len;
    int ret = 2;
    u64 config_programid;
    char *strptr;
    char *exploitname, *titlename;
    char line[256];

    f = fopen(filepath, "r");
    if(f==NULL) return 1;

    memset(line, 0, sizeof(line));
    while(fgets(line, sizeof(line) - 1, f))
    {
        remove_newline(line);

        len = strlen(line);
        if(len == 0) continue;

        strptr = strtok(line, " ");
        if(strptr == NULL) continue;
        exploitname = strptr;

        strptr = strtok(NULL, " ");
        if(strptr == NULL) continue;
        titlename = strptr;

        strptr = strtok(NULL, " ");
        if(strptr == NULL) continue;
        *out_flags_bitmask = 0;
        sscanf(strptr, "0x%lx", out_flags_bitmask);

        while((strptr = strtok(NULL, " ")))
        {
            config_programid = 0;
            sscanf(strptr, "%016llx", &config_programid);
            if(config_programid == 0) continue;

            if(*cur_programid == config_programid)
            {
                ret = 0;
                break;
            }
        }

        if(ret == 0) break;
    }

    fclose(f);

    if(ret == 0)
    {
        strncpy(out_exploitname, exploitname, 63);
        strncpy(out_titlename, titlename, 63);
    }

    return ret;
}

Result load_exploitversion(char *exploitname, u64 *cur_programid, int index, u32* out_remaster, char* out_displayversion)
{
    int ret = 2;

    int len;
    char *strptr;
    char *namestr = NULL, *valuestr = NULL;

    char filepath[256] = {0};
    char line[256] = {0};
    snprintf(filepath, sizeof(filepath) - 1, "romfs:/%s/%016llx/config.ini", exploitname, *cur_programid);

    int stage = 0;
    int i = 0;

    FILE* f = fopen(filepath, "r");
    if(f == NULL) return 1;

    while(fgets(line, sizeof(line) - 1, f))
    {
        remove_newline(line);

        len = strlen(line);
        if(len == 0) continue;

        if(stage == 0)
        {
            if(strcmp(line, "[remaster_versions]") == 0)
            {
                ret = 3;
                stage = 1;
            }
        }
        else if(stage == 1)
        {
            if(i != index)
            {
                i++;
                continue;
            }

            strptr = strtok(line, "=");
            if(strptr == NULL) continue;
            namestr = strptr;

            strptr = strtok(NULL, "=");
            if(strptr == NULL) continue;
            valuestr = strptr;

            unsigned int tmpremaster = 0;
            if(sscanf(namestr, "%04X", &tmpremaster) == 1)
            {
                ret = 4;

                strptr = strtok(valuestr, "@");
                if(strptr == NULL) break;

                strptr = strtok(NULL, "@");
                if(strptr == NULL) break;

                if(out_displayversion) strncpy(out_displayversion, strptr, 63);
                if(out_remaster) *out_remaster = tmpremaster;

                ret = 0;

                break;
            }
        }
    }

    fclose(f);

    return ret;
}

Result load_exploitconfig(char *exploitname, u64 *cur_programid, u32 app_remaster_version, u16 *update_titleversion, u32 *installed_remaster_version, char *out_versiondir, char *out_displayversion)
{
    FILE *f;
    int len;
    int ret = 2;
    int stage = 0;
    unsigned int tmpver, tmpremaster;
    char *strptr;
    char *namestr = NULL, *valuestr = NULL;
    char filepath[256];
    char line[256];

    if(update_titleversion == NULL)
    {
        *installed_remaster_version = app_remaster_version;
        stage = 2;
        ret = 5;
    }

    memset(filepath, 0, sizeof(filepath));

    snprintf(filepath, sizeof(filepath) - 1, "romfs:/%s/%016llx/config.ini", exploitname, *cur_programid);

    f = fopen(filepath, "r");
    if(f == NULL) return 1;

    memset(line, 0, sizeof(line));
    while(fgets(line, sizeof(line) - 1, f))
    {
        remove_newline(line);

        len = strlen(line);
        if(len == 0) continue;

        if(stage == 1 || stage == 3)
        {
            strptr = strtok(line, "=");
            if(strptr == NULL) continue;
            namestr = strptr;

            strptr = strtok(NULL, "=");
            if(strptr == NULL) continue;
            valuestr = strptr;
        }

        if(stage == 0)
        {
            if(strcmp(line, "[updatetitle_versions]") == 0)
            {
                ret = 3;
                stage = 1;
            }
        }
        else if(stage == 1)
        {
            tmpver = 0;
            tmpremaster = 0;
            if(sscanf(namestr, "v%u", &tmpver) == 1)
            {
                if(sscanf(valuestr, "%04X", &tmpremaster) == 1)
                {
                    if(tmpver == *update_titleversion)
                    {
                        if(app_remaster_version < tmpremaster)
                        {
                            *installed_remaster_version = tmpremaster;
                        }
                        else
                        {
                            *installed_remaster_version = app_remaster_version;
                        }

                        ret = 4;
                        stage = 2;
                        fseek(f, 0, SEEK_SET);
                    }
                }
            }
        }
        else if(stage == 2)
        {
            if(strcmp(line, "[remaster_versions]") == 0)
            {
                ret = 5;
                stage = 3;
            }
        }
        else if(stage == 3)
        {
            tmpremaster = 0;
            if(sscanf(namestr, "%04X", &tmpremaster) == 1)
            {
                if(*installed_remaster_version == tmpremaster)
                {
                    ret = 4;
                    strptr = strtok(valuestr, "@");
                    if(strptr == NULL) break;

                    strncpy(out_versiondir, strptr, 63);

                    strptr = strtok(NULL, "@");
                    if(strptr == NULL) break;
                    strncpy(out_displayversion, strptr, 63);

                    ret = 0;

                    break;
                }
            }
        }
    }

    fclose(f);

    return ret;
}

Result convert_filepath(char *inpath, char *outpath, u32 outpath_maxsize, int selected_slot)
{
    char *strptr = NULL;
    char *convstr = NULL;
    char tmpstr[8];
    char tmpstr2[16];

    strptr = strtok(inpath, "@");

    while(strptr)
    {
        convstr = &strptr[strlen(strptr) + 1];

        strncat(outpath, strptr, outpath_maxsize - 1);

        if(convstr[0] != '!')
        {
            strptr = strtok(NULL, "@");
            continue;
        }

        switch(convstr[1])
        {
            case 'd':
            {
                if(convstr[2] < '0' || convstr[2] > '9') return 9;

                memset(tmpstr, 0, sizeof(tmpstr));
                memset(tmpstr2, 0, sizeof(tmpstr2));
                snprintf(tmpstr, sizeof(tmpstr) - 1, "%s%c%c", "%0", convstr[2], convstr[1]);
                snprintf(tmpstr2, sizeof(tmpstr2) - 1, tmpstr, selected_slot);

                strncat(outpath, tmpstr2, outpath_maxsize - 1);

                strptr = strtok(&convstr[3], "@");
                break;
            }

            case 'p':
            {
                char tmpstr3[9];
                for(int i = 0; i < 8; i++)
                {
                    tmpstr3[i] = convstr[i + 2];
                    if(!isxdigit(tmpstr3[i])) return 9;
                }

                payload_embed.offset = strtol(tmpstr3, NULL, 16);
                payload_embed.enabled = true;

                strptr = strtok(&convstr[10], "@");
                strncpy(payload_embed.path, outpath, sizeof(payload_embed.path) - 1);
                break;
            }

            default: return 9;
        }
    }

    return 0;
}

Result parsecopy_saveconfig(char *versiondir, u32 type, int selected_slot)
{
    FILE *f, *fsave;
    int fd=0;
    int len;
    int ret = 2;
    u8 *savebuffer;
    u32 savesize;
    char *strptr;
    char *namestr, *valuestr;
    u32 tmpval=0;
    struct stat filestats;
    char line[256];
    char tmpstr[256];
    char tmpstr2[256];
    char savedir[256];

    memset(savedir, 0, sizeof(savedir));
    memset(tmpstr, 0, sizeof(tmpstr));

    if(type < 2)
        snprintf(savedir, sizeof(savedir) - 1, "%s/%s", versiondir, type == 0 ? "Old3DS" : "New3DS");
    else
        snprintf(savedir, sizeof(savedir) - 1, "%s/%s", versiondir, "common");

    snprintf(tmpstr, sizeof(tmpstr) - 1, "%s/%s", savedir, "config.ini");

    f = fopen(tmpstr, "r");
    if(f == NULL) return 1;

    memset(line, 0, sizeof(line));
    while(fgets(line, sizeof(line) - 1, f))
    {
        remove_newline(line);

        len = strlen(line);
        if(len == 0) continue;

        strptr = strtok(line, "=");
        if(strptr == NULL) break;
        namestr = strptr;

        strptr = strtok(NULL, "=");
        if(strptr == NULL) break;
        valuestr = strptr;

        memset(tmpstr2, 0, sizeof(tmpstr2));

        ret = convert_filepath(namestr, tmpstr2, sizeof(tmpstr2), selected_slot);
        if(ret) break;

        memset(tmpstr, 0, sizeof(tmpstr));
        snprintf(tmpstr, sizeof(tmpstr) - 1, "%s/%s", savedir, tmpstr2);

        fsave = fopen(tmpstr, "r");
        if(fsave == NULL)
        {
            ret = 3;
            break;
        }

        fd = fileno(fsave);
        if(fd == -1)
        {
            fclose(fsave);
            ret = errno;
            break;
        }

        if(fstat(fd, &filestats) == -1)
        {
            fclose(fsave);
            ret = errno;
            break;
        }

        savesize = filestats.st_size;
        if(savesize == 0)
        {
            fclose(fsave);
            ret = 4;
            break;
        }

        savebuffer = malloc(savesize);
        if(savebuffer == NULL)
        {
            fclose(fsave);
            ret = 5;
            break;
        }

        tmpval = fread(savebuffer, 1, savesize, fsave);
        fclose(fsave);
        if(tmpval != savesize)
        {
            ret = 6;
            free(savebuffer);
            break;
        }

        memset(tmpstr2, 0, sizeof(tmpstr2));

        ret = convert_filepath(valuestr, tmpstr2, sizeof(tmpstr2), selected_slot);
        if(ret)
        {
            free(savebuffer);
            break;
        }

        ret = write_savedata(tmpstr2, savebuffer, savesize);
        free(savebuffer);

        if(ret) break;
    }

    fclose(f);

    return ret;
}

int main()
{
    gfxInitDefault();
    gfxSet3D(false);

    PrintConsole topConsole, botConsole;
    consoleInit(GFX_TOP, &topConsole);
    consoleInit(GFX_BOTTOM, &botConsole);

    consoleSelect(&topConsole);
    consoleClear();

    state_t current_state = STATE_NONE;
    state_t next_state = STATE_INITIALIZE;

    FS_ProductInfo product_info;

    char exploitname[64] = {0};
    char titlename[64] = {0};

    char versiondir[64] = {0};
    char displayversion[64] = {0};

    u32 flags_bitmask = 0;

    static char top_text[2048];
    char top_text_tmp[256];
    top_text[0] = '\0';

    int firmware_version[6] = {0};
    int firmware_selected_value = 0;

    int selected_slot = 0;
    int selected_version = 0;
    u32 selected_remaster = 0;

    AM_TitleEntry update_title;
    bool update_exists = false;
    int version_maxnum = 0;

    void* payload_buffer = NULL;
    size_t payload_size = 0;

    u64 program_id = 0;

    while(aptMainLoop())
    {
        hidScanInput();
        if(hidKeysDown() & KEY_START) break;

        // transition function
        if(next_state != current_state)
        {
            memset(top_text_tmp, 0, sizeof(top_text_tmp));

            switch(next_state)
            {
                case STATE_INITIALIZE:
                    strncat(top_text, "Initializing... You may press START at any time\nto return to menu.\n\n", sizeof(top_text) - 1);
                    break;
                case STATE_INITIAL:
                    strncat(top_text, "Welcome to sploit_installer: SALT edition!\nPlease proceed with caution, as you might lose\ndata if you don't.\n\nPress A to continue.\n\n", sizeof(top_text) - 1);
                    break;
                case STATE_SELECT_VERSION:
                    snprintf(top_text_tmp, sizeof(top_text_tmp) - 1, "Auto-detected %s version: %s\nD-Pad to select, A to continue.\n\n", titlename, displayversion);
                    break;
                case STATE_SELECT_SLOT:
                    snprintf(top_text_tmp, sizeof(top_text_tmp) - 1, "Please select the savegame slot %s will be\ninstalled to. D-Pad to select, A to continue.\n", exploitname);
                    break;
                case STATE_SELECT_FIRMWARE:
                    strncat(top_text, "Please select your console's firmware version.\nOnly select NEW 3DS if you own a New 3DS (XL).\nD-Pad to select, A to continue.\n", sizeof(top_text) - 1);
                    break;
                case STATE_DOWNLOAD_PAYLOAD:
                    snprintf(top_text, sizeof(top_text) - 1, "%s\n\n\nDownloading payload...\n", top_text);
                    break;
                case STATE_COMPRESS_PAYLOAD:
                    strncat(top_text, "Processing payload...\n", sizeof(top_text) - 1);
                    break;
                case STATE_INSTALL_PAYLOAD:
                    strncat(top_text, "Installing payload...\n\n", sizeof(top_text) - 1);
                    break;
                case STATE_INSTALLED_PAYLOAD:
                    snprintf(top_text_tmp, sizeof(top_text_tmp) - 1, "Done!\n%s was successfully installed.", exploitname);
                    break;
                case STATE_ERROR:
                    strncat(top_text, "Looks like something went wrong. :(\n", sizeof(top_text) - 1);
                    break;
                default:
                    break;
            }

            if(top_text_tmp[0]) strncat(top_text, top_text_tmp, sizeof(top_text) - 1);

            current_state = next_state;
        }

        consoleSelect(&topConsole);
        printf("\x1b[0;%dHsploit_installer: SALT edition\n\n\n", (50 - 31) / 2);
        printf(top_text);

        // state function
        switch(current_state)
        {
            case STATE_INITIALIZE:
                {
                    fsInit();

                    // get an fs:USER session as the game
                    Result ret = srvGetServiceHandleDirect(&save_session, "fs:USER");
                    if(R_SUCCEEDED(ret)) ret = FSUSER_Initialize(save_session);
                    if(R_FAILED(ret))
                    {
                        snprintf(status, sizeof(status) - 1, "Failed to get game fs:USER session.\n    Error code: %08lX", ret);
                        next_state = STATE_ERROR;
                        break;
                    }

                    ret = httpcInit(0);
                    if(R_FAILED(ret))
                    {
                        snprintf(status, sizeof(status) - 1, "Failed to initialize httpc.\n    Error code: %08lX", ret);
                        next_state = STATE_ERROR;
                        break;
                    }

                    OS_VersionBin nver_versionbin, cver_versionbin;
                    ret = osGetSystemVersionData(&nver_versionbin, &cver_versionbin);
                    if(R_FAILED(ret))
                    {
                        snprintf(status, sizeof(status) - 1, "Failed to get the system version.\n    Error code: %08lX", ret);
                        next_state = STATE_ERROR;
                        break;
                    }

                    ret = cfguInit();
                    if(R_FAILED(ret))
                    {
                        snprintf(status, sizeof(status) - 1, "Failed to initialize cfgu.\n    Error code: %08lX", ret);
                        next_state = STATE_ERROR;
                        break;
                    }

                    u8 region = 0;
                    ret = CFGU_SecureInfoGetRegion(&region);
                    if(R_FAILED(ret))
                    {
                        snprintf(status, sizeof(status) - 1, "Failed to get the system region.\n    Error code: %08lX", ret);
                        next_state = STATE_ERROR;
                        break;
                    }

                    cfguExit();

                    bool is_new3ds = false;
                    APT_CheckNew3DS(&is_new3ds);

                    firmware_version[0] = is_new3ds;
                    firmware_version[5] = region;

                    firmware_version[1] = cver_versionbin.mainver;
                    firmware_version[2] = cver_versionbin.minor;
                    firmware_version[3] = cver_versionbin.build;
                    firmware_version[4] = nver_versionbin.mainver;

                    u32 pid = 0;
                    ret = svcGetProcessId(&pid, CUR_PROCESS_HANDLE);
                    if(R_FAILED(ret))
                    {
                        snprintf(status, sizeof(status) - 1, "Failed to get the process ID for the current process.\n    Error code: %08lX", ret);
                        next_state = STATE_ERROR;
                        break;
                    }

                    ret = FSUSER_GetProductInfo(&product_info, pid);
                    selected_remaster = product_info.remasterVersion;
                    if(R_FAILED(ret))
                    {
                        snprintf(status, sizeof(status) - 1, "Failed to get the product info for the current process.\n    Error code: %08lX", ret);
                        next_state = STATE_ERROR;
                        break;
                    }

                    ret = APT_GetProgramID(&program_id);
                    if(R_FAILED(ret))
                    {
                        snprintf(status, sizeof(status) - 1, "Failed to get the program ID for the current process.\n    Error code: %08lX", ret);
                        next_state = STATE_ERROR;
                        break;
                    }

                    u64 update_program_id = 0;
                    if(((program_id >> 32) & 0xFFFF) == 0) update_program_id = program_id | 0x0000000E00000000ULL;

                    if(update_program_id)
                    {
                        ret = amInit();
                        if(R_FAILED(ret))
                        {
                            snprintf(status, sizeof(status) - 1, "Failed to initialize AM.\n    Error code: %08lX", ret);
                            next_state = STATE_ERROR;
                            break;
                        }

                        ret = AM_GetTitleInfo(1, 1, &update_program_id, &update_title);
                        amExit();

                        if(R_SUCCEEDED(ret))
                            update_exists = true;
                    }

                    ret = romfsInit();
                    if(R_FAILED(ret))
                    {
                        snprintf(status, sizeof(status) - 1, "Failed to initialize romfs for this application (romfsInit()).\n    Error code: %08lX", ret);
                        next_state = STATE_ERROR;
                        break;
                    }

                    ret = load_exploitlist_config("romfs:/exploitlist_config", &program_id, exploitname, titlename, &flags_bitmask);
                    if(ret)
                    {
                        snprintf(status, sizeof(status) - 1, "Failed to select the exploit.\n    Error code: %08lX", ret);
                        if(ret == 1) strncat(status, " Failed to\nopen the config file in romfs.", sizeof(status) - 1);
                        if(ret == 2) strncat(status, " This title is not supported.", sizeof(status) - 1);
                        next_state = STATE_ERROR;
                        break;
                    }

                    int version_index = 0;
                    u32 this_remaster = 0;
                    char this_displayversion[64] = {0};
                    while(true)
                    {
                        ret = load_exploitversion(exploitname, &program_id, version_index, &this_remaster, this_displayversion);
                        if(ret) break;

                        if(this_remaster == selected_remaster)
                        {
                            strncpy(displayversion, this_displayversion, 63);
                            selected_version = version_index;
                        }

                        version_index++;
                    }

                    if(version_index == 0)
                    {
                        snprintf(status, sizeof(status) - 1, "Failed to read remaster versions from config.");
                        next_state = STATE_ERROR;
                        break;
                    }

                    version_maxnum = version_index - 1;
                    next_state = STATE_INITIAL;
                }
                break;

            case STATE_INITIAL:
                {
                    if(hidKeysDown() & KEY_A)
                    {
                        if(version_maxnum != 0) next_state = STATE_SELECT_VERSION;
                        else if(flags_bitmask & 0x10) next_state = STATE_SELECT_FIRMWARE;
                        else next_state = STATE_SELECT_SLOT;
                    }
                }
                break;

            case STATE_SELECT_VERSION:
                {
                    if(hidKeysDown() & KEY_UP) selected_version++;
                    if(hidKeysDown() & KEY_DOWN) selected_version--;
                    if(hidKeysDown() & KEY_A)
                    {
                        if(flags_bitmask & 0x10) next_state = STATE_SELECT_FIRMWARE;
                        else next_state = STATE_SELECT_SLOT;
                    }

                    if(selected_version < 0) selected_version = 0;
                    if(selected_version > version_maxnum) selected_version = version_maxnum;

                    Result ret = load_exploitversion(exploitname, &program_id, selected_version, &selected_remaster, displayversion);
                    if(ret)
                    {
                        snprintf(status, sizeof(status) - 1, "Failed to read remaster version from config.");
                        next_state = STATE_ERROR;
                        break;
                    }

                    printf((selected_version >= version_maxnum) ? "                       \n" : "                      ^\n");
                    printf("      Selected version: %s  \n", displayversion);
                    printf((!selected_version) ? "                       \n" : "                      v\n");
                }
                break;

            case STATE_SELECT_SLOT:
                {
                    if(hidKeysDown() & KEY_UP) selected_slot++;
                    if(hidKeysDown() & KEY_DOWN) selected_slot--;
                    if(hidKeysDown() & KEY_A) next_state = STATE_SELECT_FIRMWARE;

                    if(selected_slot < 0) selected_slot = 0;
                    if(selected_slot > 2) selected_slot = 2;

                    printf((selected_slot >= 2) ? "                                             \n" : "                                            ^\n");
                    printf("                            Selected slot: %d  \n", selected_slot + 1);
                    printf((!selected_slot) ? "                                             \n" : "                                            v\n");
                }
                break;

            case STATE_SELECT_FIRMWARE:
                {
                    if(hidKeysDown() & KEY_LEFT) firmware_selected_value--;
                    if(hidKeysDown() & KEY_RIGHT) firmware_selected_value++;

                    if(firmware_selected_value < 0) firmware_selected_value = 0;
                    if(firmware_selected_value > 5) firmware_selected_value = 5;

                    if(hidKeysDown() & KEY_UP) firmware_version[firmware_selected_value]++;
                    if(hidKeysDown() & KEY_DOWN) firmware_version[firmware_selected_value]--;

                    int firmware_maxnum = 256;
                    if(firmware_selected_value == 0) firmware_maxnum = 2;
                    if(firmware_selected_value == 5) firmware_maxnum = 7;

                    if(firmware_version[firmware_selected_value] < 0) firmware_version[firmware_selected_value] = 0;
                    if(firmware_version[firmware_selected_value] >= firmware_maxnum) firmware_version[firmware_selected_value] = firmware_maxnum - 1;

                    if(hidKeysDown() & KEY_A) next_state = STATE_DOWNLOAD_PAYLOAD;

                    int offset = 26;
                    if(firmware_selected_value)
                    {
                        offset += 7;

                        for(int i = 1; i < firmware_selected_value; i++)
                        {
                            offset += 2;
                            if(firmware_version[i] >= 10) offset++;
                        }
                    }

                    printf((firmware_version[firmware_selected_value] < firmware_maxnum - 1) ? "%*s^%*s" : "%*s-%*s", offset, " ", 50 - offset - 1, " ");
                    printf("      Selected firmware: %s %d-%d-%d-%d %s  \n", firmware_version[0] ? "New3DS" : "Old3DS", firmware_version[1], firmware_version[2], firmware_version[3], firmware_version[4], regions[firmware_version[5]]);
                    printf((firmware_version[firmware_selected_value] > 0) ? "%*sv%*s" : "%*s-%*s", offset, " ", 50 - offset - 1, " ");
                }
                break;

            case STATE_DOWNLOAD_PAYLOAD:
                {
                    httpcContext context;
                    static char in_url[512];
                    static char out_url[512];

                    snprintf(in_url, sizeof(in_url) - 1, "http://smea.mtheall.com/get_payload.php?version=%s-%d-%d-%d-%d-%s",
                        firmware_version[0] ? "NEW" : "OLD", firmware_version[1], firmware_version[2], firmware_version[3], firmware_version[4], regions[firmware_version[5]]);

                    char user_agent[64];
                    snprintf(user_agent, sizeof(user_agent) - 1, "salt_sploit_installer-%s", exploitname);
                    Result ret = get_redirect(in_url, out_url, 512, user_agent);
                    if(R_FAILED(ret))
                    {
                        sprintf(status, "Failed to grab payload url\n    Error code: %08lX", ret);
                        next_state = STATE_ERROR;
                        break;
                    }

                    ret = httpcOpenContext(&context, HTTPC_METHOD_GET, out_url, 0);
                    if(R_FAILED(ret))
                    {
                        sprintf(status, "Failed to open http context\n    Error code: %08lX", ret);
                        next_state = STATE_ERROR;
                        break;
                    }

                    ret = download_file(&context, &payload_buffer, &payload_size, user_agent);
                    if(R_FAILED(ret))
                    {
                        sprintf(status, "Failed to download payload\n    Error code: %08lX", ret);
                        next_state = STATE_ERROR;
                        break;
                    }

                    if(flags_bitmask & 0x1) next_state = STATE_COMPRESS_PAYLOAD;
                    else next_state = STATE_INSTALL_PAYLOAD;
                }
                break;

            case STATE_COMPRESS_PAYLOAD:
                payload_buffer = BLZ_Code(payload_buffer, payload_size, &payload_size, BLZ_NORMAL);
                next_state = STATE_INSTALL_PAYLOAD;
                break;

            case STATE_INSTALL_PAYLOAD:
                {
                    u32 selected_remaster_version = 0;
                    Result ret = load_exploitconfig(exploitname, &program_id, selected_remaster, update_exists ? &update_title.version : NULL, &selected_remaster_version, versiondir, displayversion);
                    if(ret)
                    {
                        snprintf(status, sizeof(status) - 1, "Failed to find your version of\n%s in the config / config loading failed.\n    Error code: %08lX", titlename, ret);
                        if(ret == 1) strncat(status, " Failed to\nopen the config file in romfs.", sizeof(status) - 1);
                        if(ret == 2 || ret == 4) strncat(status, " The romfs config file is invalid.", sizeof(status) - 1);
                        if(ret == 3)
                        {
                            snprintf(status, sizeof(status) - 1, "this update-title version (v%u) of %s is not compatible with %s, sorry\n", update_title.version, titlename, exploitname);
                            next_state = STATE_ERROR;
                            break;
                        }
                        if(ret == 5)
                        {
                            snprintf(status, sizeof(status) - 1, "this remaster version (%04lX) of %s is not compatible with %s, sorry\n", selected_remaster_version, titlename, exploitname);
                            next_state = STATE_ERROR;
                            break;
                        }

                        next_state = STATE_ERROR;
                        break;
                    }

                    if(flags_bitmask & 0x8)
                    {
                        fsUseSession(save_session);
                        Result ret = FSUSER_FormatSaveData(ARCHIVE_SAVEDATA, (FS_Path){PATH_EMPTY, 1, (u8*)""}, 0x200, 10, 10, 11, 11, true);
                        fsEndUseSession();
                        if(ret)
                        {
                            sprintf(status, "Failed to format savedata.\n    Error code: %08lX", ret);
                            next_state = STATE_ERROR;
                            break;
                        }
                    }

                    if(flags_bitmask & 0x2)
                    {
                        Result ret = parsecopy_saveconfig(versiondir, firmware_version[0], selected_slot);
                        if(ret)
                        {
                            sprintf(status, "Failed to install the savefiles with romfs %s savedir.\n    Error code: %08lX", firmware_version[0] == 0?"Old3DS" : "New3DS", ret);
                            next_state = STATE_ERROR;
                            break;
                        }
                    }

                    if(flags_bitmask & 0x4)
                    {
                        Result ret = parsecopy_saveconfig(versiondir, 2, selected_slot);
                        if(ret)
                        {
                            sprintf(status, "Failed to install the savefiles with romfs %s savedir.\n    Error code: %08lX", "common", ret);
                            next_state = STATE_ERROR;
                            break;
                        }
                    }
                }

                {
                    Result ret;

                    if(payload_embed.enabled)
                    {
                        void* buffer = NULL;
                        size_t size = 0;
                        ret = read_savedata(payload_embed.path, &buffer, &size);
                        if(ret)
                        {
                            sprintf(status, "Failed to embed payload\n    Error code: %08lX", ret);
                            next_state = STATE_ERROR;
                            break;
                        }
                        if((payload_embed.offset + payload_size + sizeof(u32)) >= size)
                        {
                            sprintf(status, "Failed to embed payload (too large)\n    0x%X >= 0x%X", (payload_embed.offset + payload_size + sizeof(u32)), size);
                            next_state = STATE_ERROR;
                            break;
                        }

                        *(u32*)(buffer + payload_embed.offset) = payload_size;
                        memcpy(buffer + payload_embed.offset + sizeof(u32), payload_buffer, payload_size);
                        ret = write_savedata(payload_embed.path, buffer, size);

                        free(buffer);
                    }
                    else
                        ret = write_savedata("/payload.bin", payload_buffer, payload_size);

                    if(ret)
                    {
                        sprintf(status, "Failed to install payload\n    Error code: %08lX", ret);
                        next_state = STATE_ERROR;
                        break;
                    }

                    next_state = STATE_INSTALLED_PAYLOAD;
                }
                break;

            case STATE_INSTALLED_PAYLOAD:
                next_state = STATE_NONE;
                break;

            default: break;
        }

        consoleSelect(&botConsole);
        printf("\x1b[0;0H  Current status:\n    %s\n", status);

        gspWaitForVBlank();
    }

    if(payload_buffer) free(payload_buffer);

    romfsExit();
    httpcExit();

    svcCloseHandle(save_session);
    fsExit();

    gfxExit();
    return 0;
}
