
/* Private includes --------------------------------------------------------------------------------------------------*/

#include <stddef.h> // for NULL

#include "mxc_delay.h"
#include "mxc_device.h"
#include "sdhc_lib.h"
#include "sdhc_regs.h"
#include <mxc_errors.h>
#include "sdhc.h"

#include "bsp_sdhc.h"
#include "sd_card.h"

#include "system_config.h"

#include "wav_header.h"

/* Private defines ---------------------------------------------------------------------------------------------------*/

#define SD_CARD_INIT_NUM_RETRIES (20)

#define SDHC_CONFIG_BLOCK_GAP (0)
#define SDHC_CONFIG_CLK_DIV (0x0b0)

/* Private variables -------------------------------------------------------------------------------------------------*/

static FATFS *fs; // FFat Filesystem Object
static FATFS fs_obj;
static FIL SD_file; // FFat File Object
static bool is_mounted = false;

static char volume = '0';

DWORD lba,lba_end; // sector numbers on file system
uint32_t SD_host_control_reg=0; // save the FATFS control reg for use later in the low-level writes
#define SDHC_CMD_ERASE_WR_BLK_START_ADDR (32)
#define SDHC_CMD_ERASE_WR_BLK_END_ADDR   (33)
#define SDHC_CMD_ERASE                   (38)

/* Public function definitions ---------------------------------------------------------------------------------------*/

int sd_card_init()
{
    int res = E_NO_ERROR;

    if ((res = bsp_sdhc_init()) != E_NO_ERROR)
    {
        return res;
    }

    // without a delay here the next function was consistently returning an error
    MXC_Delay(100000);

    if ((res = MXC_SDHC_Lib_InitCard(SD_CARD_INIT_NUM_RETRIES)) != E_NO_ERROR)
    {
        return res;
    }

    return E_NO_ERROR;
}

int sd_card_mount()
{
    fs = &fs_obj;

    if (f_mount(fs, "", 1) != FR_OK)
    {
        return E_COMM_ERR;
    }

    is_mounted = true;
    return E_NO_ERROR;
}

int sd_card_unmount()
{
    is_mounted = false;
    return f_mount(0, "", 0) == FR_OK ? E_NO_ERROR : E_COMM_ERR;
}

bool sd_card_is_mounted()
{
    return is_mounted;
}

QWORD sd_card_disk_size_bytes()
{
    if (!sd_card_is_mounted())
    {
        return 0;
    }

    // from elm-chan: http://elm-chan.org/fsw/ff/doc/getfree.html
    DWORD total_sectors = (fs->n_fatent - 2) * fs->csize;
    
    return ((QWORD)(total_sectors / 2) * (QWORD)(1024)); // for cards over 3GB, we need QWORD to hold size
}

QWORD sd_card_free_space_bytes()
{
    if (!sd_card_is_mounted())
    {
        return 0;
    }

    // from e]lm-chan: http://elm-chan.org/fsw/ff/doc/getfree.html
    QWORD free_clusters;
    if (f_getfree(&volume, &free_clusters, &fs) != FR_OK)
    {
        return 0;
    }

    DWORD free_sectors = free_clusters * fs->csize;
    return ((QWORD)(free_sectors / 2) * (QWORD)(1024));
}

int sd_card_mkdir(const char *path)
{
    return f_mkdir(path) == FR_OK ? E_NO_ERROR : E_COMM_ERR;
}

int sd_card_cd(const char *path)
{
    return f_chdir(path) == FR_OK ? E_NO_ERROR : E_COMM_ERR;
}

int sd_card_fopen_writeHeader_preErase(const char *file_name, POSIX_FileMode_t mode)
{
    mxc_sdhc_cmd_cfg_t req;
    Wave_Header_Attributes_t *wav_attr;
    uint32_t bytes_written;
    // calculate filesize, remeber that the WAV heaader was expanded to 512 for sector alignment (normally would be 44)
    
    FSIZE_t filesize =  SYS_CONFIG_FILE_LEN_BYTES; // includes extra 512 for header
    //FSIZE_t filesize = 2764800044;
    //return f_open(&SD_file, file_name, mode) == FR_OK ? E_NO_ERROR : E_COMM_ERR;
    FRESULT fr = f_open(&SD_file, file_name, mode);
    fr = f_expand(&SD_file,filesize,1); // pre-allocate contiguous sectors so it doesn't happen on-the-fly
    if(fr != FR_OK) 
    {
        return(E_COMM_ERR);
    } else {
        return(E_NO_ERROR);
    }
    SD_host_control_reg = MXC_SDHC_Get_Host_Cn_1(); // store the control reg so we can re-use it for low-level commands (it's part of the struct that is needed)
    
    // write the header; we know the length ahead of time
    wav_attr->file_length = filesize;
    uint32_t numSectors = SYS_CONFIG_NUM_SECTORS;
   
    wav_header_set_attributes(wav_attr);

    if (sd_card_fwrite(wav_header_get_header(), wav_header_get_header_length(), &bytes_written) != E_NO_ERROR)
    {
        printf("[ERROR]--> SD card WAV header fwrite\n");
    }
    else
    {
        printf("[SUCCESS]--> SD card WAV header fwrite\n");
    }

    f_sync(&SD_file);
    // Get LBA for subsequent direct block writes
    f_lseek(&SD_file, 0);
    //f_tell(&SD_file);
    lba = get_file_first_lba(&SD_file); // required to start a block erase
    lba_end = lba + numSectors-1; // lba of the last sector in the file

    // pre-erase for speed
    low_level_erase_sectors(lba+1,lba_end); // erase starting at block lba+1 so we don't erase the WAV header

// leave card open for data sector writes starting at lba+1

    // if (sd_card_fclose() != E_NO_ERROR)
    // {
    //     printf("[ERROR]--> SD card fclose\n");
    // }
    // else
    // {
    //     printf("[SUCCESS]--> SD card fclose\n");
    // }


 
}


/**
 * @brief Finds the LBA of the first block (sector) of an open file using manual calculation.
 * @param[in] fp Pointer to an open file object (FIL).
 * @return Returns the LBA of the first block, or 0xFFFFFFFF if an error occurs.
 *         (Using 0xFFFFFFFF as an invalid LBA, as LBA 0 is a valid address).
 */
LBA_t get_file_first_lba(FIL* fp) {
    // Check if the file object and its associated filesystem object are valid
    if (!fp || !fp->obj.fs) { // Access FATFS via fp->obj.fs
        printf("Error: Invalid file or filesystem object.\n");
        return 0xFFFFFFFF; // Return invalid LBA
    }

    // Get the starting cluster number from the file object's obj member
    DWORD first_cluster = fp->obj.sclust;

    // Cluster numbers 0 and 1 are reserved; data clusters start from 2.
    if (first_cluster < 2) {
        printf("Error: File's starting cluster (%lu) is invalid.\n", (unsigned long)first_cluster);
        return 0xFFFFFFFF;
    }

    // Get the LBA of the volume's data area (where cluster 2 starts).
    // Access FATFS via fp->obj.fs
    uint32_t data_start_sector = fp->obj.fs->database; 

    // Get the number of sectors per cluster.
    // Access FATFS via fp->obj.fs
    uint32_t sectors_per_cluster = fp->obj.fs->csize;
    
    // Check for a valid cluster size
    if (sectors_per_cluster == 0) {
        printf("Error: Invalid cluster size.\n");
        return 0xFFFFFFFF;
    }

    // Calculate the LBA of the first sector of the file's first cluster.
    // (first_cluster - 2) accounts for the reserved clusters 0 and 1.
    LBA_t first_block_lba = data_start_sector + (first_cluster - 2) * sectors_per_cluster;

    return first_block_lba;
}



/**
 * @brief Performs a low-level erase of a contiguous block of sectors on the SD card.
 * @param[in] start_lba   The starting logical block address (LBA).
 * @param[in] end_lba     The ending LBA.
 * @return Returns MXC_SDHC_ERR_SUCCESS on success, error code otherwise.
 */
int low_level_erase_sectors(uint32_t start_lba, uint32_t end_lba)
{
    mxc_sdhc_cmd_cfg_t cmd_cfg;
    int result;

    // --- Step 1: Send CMD32 (Erase Write Block Start Address) ---
    memset(&cmd_cfg, 0, sizeof(mxc_sdhc_cmd_cfg_t)); // Clear the struct
    cmd_cfg.command = SDHC_CMD_ERASE_WR_BLK_START_ADDR;
    cmd_cfg.arg_1 = start_lba;
    
    // Set command flags for R1 response (check your sdhc_regs.h for specific bits)
    // The driver typically handles busy-wait for R1b responses automatically.
    // Assuming a higher-level command value or a different field handles response type.
    
    // Issue the command. The function call may differ slightly.
    result = MXC_SDHC_SendCommand(&cmd_cfg);
    if (result != E_NO_ERROR) {
        printf("Failed to send CMD32: %d\n", result);
        return result;
    }
    // TODO: Add response checking from the driver, if available.

    // --- Step 2: Send CMD33 (Erase Write Block End Address) ---
    memset(&cmd_cfg, 0, sizeof(mxc_sdhc_cmd_cfg_t)); // Clear the struct
    cmd_cfg.command = SDHC_CMD_ERASE_WR_BLK_END_ADDR;
    cmd_cfg.arg_1 = end_lba;
    
    result = MXC_SDHC_SendCommand(&cmd_cfg);
    if (result != E_NO_ERROR) {
        printf("Failed to send CMD33: %d\n", result);
        return result;
    }
    // TODO: Add response checking.

    // --- Step 3: Send CMD38 (Erase) ---
    memset(&cmd_cfg, 0, sizeof(mxc_sdhc_cmd_cfg_t)); // Clear the struct
    cmd_cfg.command = SDHC_CMD_ERASE;
    cmd_cfg.arg_1 = 0; // Reserved, must be 0
    
    // For R1b busy response, the driver will likely block internally.
    result = MXC_SDHC_SendCommand(&cmd_cfg);
    if (result != E_NO_ERROR) {
        printf("Failed to send CMD38: %d\n", result);
        return result;
    }
    // TODO: Add response checking.

    printf("Low-level erase for LBAs %lu to %lu completed.\n", (unsigned long)start_lba, (unsigned long)end_lba);

    return E_NO_ERROR;
}


int sd_card_fclose()
{
    return f_close(&SD_file) == FR_OK ? E_NO_ERROR : E_COMM_ERR;
}

int sd_card_fwrite(const void *buff, uint32_t size, uint32_t *written)
{
    return f_write(&SD_file, buff, size, (UINT *)written) == FR_OK ? E_NO_ERROR : E_COMM_ERR;
}




// SDHC driver handle



// // Write SD sectors starting at LBA

// int sdio_write_sectors(uint32_t start_lba, const uint8_t *buf,uint32_t BLOCKS_PER_WRITE) {

//     #define SECTOR_SIZE 512 // bytes
//     //#define BLOCKS_PER_WRITE 24*1024/SECTOR_SIZE // mono 24-bit , EDIT for others!
//     #define TIMEOUT_MS 1000
//     mxc_sdhc_cmd_cfg_t sdhc_req;
//     int err;


//     memset(&sdhc_req, 0, sizeof(mxc_sdhc_cmd_cfg_t));

//     sdhc_req.command = 25;                // CMD25 = WRITE_MULTIPLE_BLOCK

//     sdhc_req.arg_1 = start_lba;         // starting block

//     sdhc_req.sdma = (uint8_t *)buf;   // DMA source

//     sdhc_req.block_size = SECTOR_SIZE;

//     sdhc_req.block_count = BLOCKS_PER_WRITE;  // 48

//     sdhc_req.direction = MXC_SDHC_DIRECTION_WRITE;

//     err = MXC_SDHC_SendCommand(&sdhc_req);  // starts DMA

//     if (err != E_NO_ERROR) return err;

//     // Wait for completion or timeout

//     // not in mdsk  err = MXC_SDHC_WaitForRequest(&sdhc_req, TIMEOUT_MS);

//     return err;

// }



int sd_card_lseek(uint32_t offset)
{
    return f_lseek(&SD_file, offset) == FR_OK ? E_NO_ERROR : E_COMM_ERR;
}

uint32_t sd_card_fsize()
{
    return f_size(&SD_file);
}

FRESULT set_file_timestamp (
    char *obj,     /* Pointer to the file name */
    int year,
    int month,
    int mday,
    int hour,
    int min,
    int sec
)
{
    FILINFO fno;

    fno.fdate = (WORD)(((year - 1980) * 512U) | month * 32U | mday);
    fno.ftime = (WORD)(hour * 2048U | min * 32U | sec / 2U);

    return f_utime(obj, &fno);
}
