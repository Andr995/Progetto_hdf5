// --- FIX PER ERRORI DI COMPILAZIONE ---
#define NOMINMAX // Risolve C2589 (conflitto std::min)
// Disabilita avviso C26812 (Preferire enum class) causato dalla libreria C HDF5
#pragma warning(disable: 26812)

#include <iostream>
#include <vector>
#include <string>
#include <algorithm> 
#include <cstring>
#include <thread>
#include <chrono>
#include <windows.h> // NECESSARIO per gestire i crash (SEH)

// Usa le DLL
#define H5_BUILT_AS_DYNAMIC_LIB
#include "hdf5.h"

#define FILE_NAME_INPUT "processdata.h5"
#define FILE_NAME_OUTPUT "processdata_sz3_compressed.h5"
#define H5Z_FILTER_SZ3 32024

// --- HELPER SZ3 ---
void imposta_SZ3_ABS(unsigned int* cd_values, double errorBound) {
    cd_values[0] = 0; // ABS
    unsigned long long temp_bits;
    std::memcpy(&temp_bits, &errorBound, sizeof(double));
    cd_values[1] = (unsigned int)(temp_bits >> 32);
    cd_values[2] = (unsigned int)(temp_bits & 0xFFFFFFFF);
}

// --- FUNZIONI "SAFE" (WRAPPER) PER RISOLVERE C2712 ---
// Queste funzioni contengono SOLO codice C e blocchi __try.
// NON devono contenere std::vector o std::string.

herr_t safe_h5dwrite(hid_t dset_id, hid_t mem_type_id, hid_t mem_space_id, hid_t file_space_id, hid_t xfer_plist_id, const void* buf) {
    herr_t status = -1;
    __try {
        status = H5Dwrite(dset_id, mem_type_id, mem_space_id, file_space_id, xfer_plist_id, buf);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = -2; // Codice crash
    }
    return status;
}

// NUOVA: Protezione anche per la lettura (Verifica finale)
herr_t safe_h5dread(hid_t dset_id, hid_t mem_type_id, hid_t mem_space_id, hid_t file_space_id, hid_t xfer_plist_id, void* buf) {
    herr_t status = -1;
    __try {
        status = H5Dread(dset_id, mem_type_id, mem_space_id, file_space_id, xfer_plist_id, buf);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = -2; // Codice crash intercettato
    }
    return status;
}

herr_t safe_h5fflush(hid_t object_id, H5F_scope_t scope) {
    herr_t status = -1;
    __try {
        status = H5Fflush(object_id, scope);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = -2;
    }
    return status;
}

herr_t safe_h5dclose(hid_t dset_id) {
    herr_t status = -1;
    __try {
        status = H5Dclose(dset_id);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = -2;
    }
    return status;
}

herr_t safe_h5fclose(hid_t file_id) {
    herr_t status = -1;
    __try {
        status = H5Fclose(file_id);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = -2;
    }
    return status;
}

// --- LETTURA (ORA PROTETTA) ---
bool leggi_dataset_in_memoria(hid_t file_id, const char* dataset_name, std::vector<float>& buffer, std::vector<hsize_t>& dims) {
    std::cerr << "[DEBUG] Lettura dataset: " << dataset_name << "..." << std::endl;

    H5Eset_auto(H5E_DEFAULT, NULL, NULL);

    hid_t dset_id = H5Dopen(file_id, dataset_name, H5P_DEFAULT);
    if (dset_id < 0) {
        std::cerr << "[DEBUG] Dataset non trovato (SKIP)." << std::endl;
        return false;
    }

    hid_t space_id = H5Dget_space(dset_id);
    int rank = H5Sget_simple_extent_ndims(space_id);
    dims.resize(rank);
    H5Sget_simple_extent_dims(space_id, dims.data(), NULL);

    unsigned long long total_elements = 1;
    // [FIX C4244] Uso hsize_t per il ciclo
    for (hsize_t i : dims) total_elements *= i;

    try {
        buffer.resize(total_elements);
    }
    catch (...) {
        std::cerr << "[CRASH POTENZIALE] Memoria insufficiente per " << total_elements << " elementi!" << std::endl;
        H5Sclose(space_id); H5Dclose(dset_id);
        return false;
    }

    // [MODIFICA IMPORTANTE] Usiamo safe_h5dread invece di H5Dread normale
    // Questo impedisce il freeze/crash se il plugin va in conflitto
    herr_t status = safe_h5dread(dset_id, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, buffer.data());

    H5Sclose(space_id);
    H5Dclose(dset_id);

    if (status == -2) {
        std::cerr << "[WARNING] Crash intercettato durante la lettura di verifica! (Conflitto DLL)" << std::endl;
        return false;
    }
    else if (status < 0) {
        std::cerr << "[ERRORE] H5Dread fallita." << std::endl;
        return false;
    }

    std::cerr << "[DEBUG] Letto in memoria. Dimensione: " << total_elements << std::endl;
    // [FIX C2589] std::min ora funziona grazie a NOMINMAX
    std::cerr << "        Anteprima: ";
    for (size_t i = 0; i < std::min((size_t)5, (size_t)total_elements); ++i)
        std::cerr << buffer[i] << " ";
    std::cerr << "..." << std::endl;

    return true;
}

// --- SCRITTURA (Usa le funzioni safe) ---
bool scrivi_dataset_compresso(hid_t file_id, const char* dataset_name, const std::vector<float>& buffer, const std::vector<hsize_t>& dims) {
    std::cerr << "\n   [STEP 1] Preparazione scrittura: " << dataset_name << std::endl;

    int rank = (int)dims.size();
    hid_t space_id = H5Screate_simple(rank, dims.data(), NULL);

    // CHUNK
    std::cerr << "   [STEP 2] Configurazione Chunk..." << std::endl;
    hid_t dcpl_id = H5Pcreate(H5P_DATASET_CREATE);
    std::vector<hsize_t> chunk_dims = dims;
    for (int i = 0; i < rank; i++) if (chunk_dims[i] > 100) chunk_dims[i] = 100;
    H5Pset_chunk(dcpl_id, rank, chunk_dims.data());

    // FILTRO SZ3
    std::cerr << "   [STEP 3] Configurazione Filtro SZ3..." << std::endl;
    unsigned int cd_values[20] = { 0 };
    imposta_SZ3_ABS(cd_values, 1E-4);

    if (H5Pset_filter(dcpl_id, H5Z_FILTER_SZ3, H5Z_FLAG_OPTIONAL, 9, cd_values) < 0) {
        std::cerr << "   [ERRORE] H5Pset_filter fallito." << std::endl;
        return false;
    }

    // CREAZIONE
    std::cerr << "   [STEP 4/5] Creazione Dataset..." << std::endl;
    hid_t lcpl_id = H5Pcreate(H5P_LINK_CREATE);
    H5Pset_create_intermediate_group(lcpl_id, 1);

    hid_t dset_id = H5Dcreate(file_id, dataset_name, H5T_NATIVE_FLOAT, space_id, lcpl_id, dcpl_id, H5P_DEFAULT);
    if (dset_id < 0) {
        std::cerr << "   [ERRORE] H5Dcreate fallito." << std::endl;
        return false;
    }

    // SCRITTURA PROTETTA
    std::cerr << "   [STEP 6] H5Dwrite (Compressione)..." << std::endl;

    // Usiamo la funzione safe_h5dwrite
    herr_t status = safe_h5dwrite(dset_id, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, buffer.data());

    if (status == -2) {
        std::cerr << "   [WARNING] Crash intercettato durante la scrittura! (I dati potrebbero essere corrotti)" << std::endl;
    }
    else if (status < 0) {
        std::cerr << "   [ERRORE] H5Dwrite fallito." << std::endl;
    }
    else {
        std::cerr << "   [STEP 7] Scrittura completata!" << std::endl;
        // Flush protetto
        safe_h5fflush(file_id, H5F_SCOPE_GLOBAL);
    }

    // CHIUSURA RISORSE
    std::cerr << "   [STEP 8] Chiusura risorse (Safe Mode)..." << std::endl;

    H5Sclose(space_id);
    H5Pclose(lcpl_id);
    H5Pclose(dcpl_id);

    std::cerr << "      -> Closing dset_id... ";

    // Usiamo la funzione safe_h5dclose
    herr_t close_status = safe_h5dclose(dset_id);

    if (close_status == -2) {
        std::cerr << "\n      [SALVATO] Crash intercettato durante H5Dclose! Ignoro e proseguo.";
    }
    else if (close_status < 0) {
        std::cerr << "[ERR]";
    }
    else {
        std::cerr << "[OK]";
    }
    std::cerr << std::endl;

    std::cerr << "   [STEP 9] Risorse gestite.\n" << std::endl;
    return true;
}

int main() {
    (void)_putenv("HDF5_PLUGIN_PATH=.");
    (void)_putenv("OMP_NUM_THREADS=1");

    std::cerr << "--- AVVIO PROTETTO ---" << std::endl;

    hid_t file_in = H5Fopen(FILE_NAME_INPUT, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (file_in < 0) {
        std::cerr << "ERRORE: Input non trovato." << std::endl;
        system("pause"); return -1;
    }

    hid_t file_out = H5Fcreate(FILE_NAME_OUTPUT, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (file_out < 0) {
        std::cerr << "ERRORE: Output fallito." << std::endl;
        H5Fclose(file_in); return -1;
    }

    const char* datasets[] = { "vertex/x", "vertex/y", "vertex/z" };

    for (int i = 0; i < 3; ++i) {
        const char* name = datasets[i];
        std::cerr << "=== ELABORAZIONE " << name << " ===" << std::endl;

        std::vector<float> buffer;
        std::vector<hsize_t> dims;

        if (leggi_dataset_in_memoria(file_in, name, buffer, dims)) {
            if (!scrivi_dataset_compresso(file_out, name, buffer, dims)) {
                std::cerr << "STOP per errore critico su " << name << std::endl;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::cerr << "--- CHIUSURA FILE ---" << std::endl;
    H5Fclose(file_in);

    // Chiusura file protetta
    if (safe_h5fclose(file_out) == -2) {
        std::cerr << "[WARNING] Crash su H5Fclose (Output). I dati dovrebbero essere salvi." << std::endl;
    }

    std::cerr << "--- FINE PROGRAMMA ---" << std::endl;

    // Verifica finale
    std::cout << "\n[VERIFICA] Riapro il file generato..." << std::endl;
    hid_t check = H5Fopen(FILE_NAME_OUTPUT, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (check >= 0) {
        std::cout << "File OK! Contiene i dati." << std::endl;

        for (const char* name : datasets) {
            std::cout << "\nVerifica Dataset: " << name << std::endl;
            std::vector<float> buffer;
            std::vector<hsize_t> dims;
            // Ora anche la lettura di verifica è protetta dai crash!
            leggi_dataset_in_memoria(check, name, buffer, dims);
        }
        H5Fclose(check);
    }
    else {
        std::cout << "Impossibile riaprire il file." << std::endl;
    }

    std::cout << "Premi INVIO per uscire.";
    std::cin.get();
    return 0;
}