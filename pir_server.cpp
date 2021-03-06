#include "pir_server.hpp"

using namespace std;
using namespace seal;
using namespace seal::util;

PIRServer::PIRServer(const EncryptionParameters &expanded_params, const PirParams &pir_params) {
    expanded_params_ = expanded_params;
    pir_params_ = pir_params;
    SEALContext context(expanded_params);
    evaluator_.reset(new Evaluator(context));
    is_db_preprocessed_ = false;
}

PIRServer::~PIRServer() {
    delete db_;
}

void PIRServer::update_parameters(const EncryptionParameters &expanded_params,
                                  const PirParams &pir_params) {

    // The only thing that can change is the plaintext modulus and pir_params
    assert(expanded_params.poly_modulus() == expanded_params_.poly_modulus());
    assert(expanded_params.coeff_modulus() == expanded_params_.coeff_modulus());

    expanded_params_ = expanded_params;
    pir_params_ = pir_params;
    SEALContext context(expanded_params);
    evaluator_.reset(new Evaluator(context));
    is_db_preprocessed_ = false;

    // Update all the galois keys
    for (std::pair<const int, GaloisKeys> &key : galoisKeys_) {
        key.second.mutable_hash_block() = expanded_params_.hash_block();
    }
}

void PIRServer::preprocess_database() {
    if (!is_db_preprocessed_) {

        for (uint32_t i = 0; i < db_->size(); i++) {
            evaluator_->transform_to_ntt(db_->operator[](i));
        }

        is_db_preprocessed_ = true;
    }
}

// Server takes over ownership of db and will free it when it exits
void PIRServer::set_database(vector<Plaintext> *db) {
    if (db == nullptr) {
        throw invalid_argument("db cannot be null");
    }

    db_ = db;
    is_db_preprocessed_ = false;
}

void PIRServer::set_database(const uint8_t *bytes, uint64_t ele_num, uint64_t ele_size) {

    uint32_t logtp = ceil(log2(expanded_params_.plain_modulus().value()));
    uint32_t N = expanded_params_.poly_modulus().coeff_count() - 1;

    // number of FV plaintexts needed to represent all elements
    uint64_t total = plaintexts_per_db(logtp, N, ele_num, ele_size);

    // number of FV plaintexts needed to create the d-dimensional matrix
    uint64_t prod = 1;
    for (uint32_t i = 0; i < pir_params_.nvec.size(); i++) {
        prod *= pir_params_.nvec[i];
    }
    uint64_t matrix_plaintexts = prod;
    assert(total <= matrix_plaintexts);

    vector<Plaintext> *result = new vector<Plaintext>();
    result->reserve(matrix_plaintexts);

    uint64_t ele_per_ptxt = elements_per_ptxt(logtp, N, ele_size);
    uint64_t bytes_per_ptxt = ele_per_ptxt * ele_size;

    uint64_t db_size = ele_num * ele_size;

    uint64_t coeff_per_ptxt = ele_per_ptxt * coefficients_per_element(logtp, ele_size);
    assert(coeff_per_ptxt <= N);

    uint32_t offset = 0;

    for (uint64_t i = 0; i < total; i++) {

        uint64_t process_bytes = 0;

        if (db_size <= offset) {
            break;
        } else if (db_size < offset + bytes_per_ptxt) {
            process_bytes = db_size - offset;
        } else {
            process_bytes = bytes_per_ptxt;
        }

        // Get the coefficients of the elements that will be packed in plaintext i
        vector<uint64_t> coefficients = bytes_to_coeffs(logtp, bytes + offset, process_bytes);
        offset += process_bytes;

        uint64_t used = coefficients.size();

        assert(used <= coeff_per_ptxt);

        // Pad the rest with 1s
        for (uint64_t j = 0; j < (N - used); j++) {
            coefficients.push_back(1);
        }

        Plaintext plain;
        vector_to_plaintext(coefficients, plain);
        result->push_back(plain);
    }

    // Add padding to make database a matrix
    uint64_t current_plaintexts = result->size();
    assert(current_plaintexts <= total);

#ifdef DEBUG
    cout << "adding: " << matrix_plaintexts - current_plaintexts
         << " FV plaintexts of padding (equivalent to: "
         << (matrix_plaintexts - current_plaintexts) * elements_per_ptxt(logtp, N, ele_size)
         << " elements)" << endl;
#endif

    vector<uint64_t> padding(N, 1);

    for (uint64_t i = 0; i < (matrix_plaintexts - current_plaintexts); i++) {
        Plaintext plain;
        vector_to_plaintext(padding, plain);
        result->push_back(plain);
    }

    set_database(result);
}

void PIRServer::set_galois_key(std::uint32_t client_id, seal::GaloisKeys galkey) {
    galkey.mutable_hash_block() = expanded_params_.hash_block();
    galoisKeys_[client_id] = galkey;
}

PirReply PIRServer::generate_reply(PirQuery query, uint32_t client_id) {

    vector<uint64_t> nvec = pir_params_.nvec;
    uint64_t product = 1;

    for (uint32_t i = 0; i < nvec.size(); i++) {
        product *= nvec[i];
    }

    int coeff_count = expanded_params_.poly_modulus().coeff_count();

    vector<Plaintext> *cur = db_;
    vector<Plaintext> intermediate_plain; // decompose....

    auto my_pool = MemoryPoolHandle::New();

    for (uint32_t i = 0; i < nvec.size(); i++) {
        uint64_t n_i = nvec[i];
        vector<Ciphertext> expanded_query = expand_query(query[i], n_i, client_id);

        // Transform expanded query to NTT, and ...
        for (uint32_t jj = 0; jj < expanded_query.size(); jj++) {
            evaluator_->transform_to_ntt(expanded_query[jj]);
        }

        // Transform plaintext to NTT. If database is pre-processed, can skip
        if ((!is_db_preprocessed_) || i > 0) {
            for (uint32_t jj = 0; jj < cur->size(); jj++) {
                evaluator_->transform_to_ntt((*cur)[jj]);
            }
        }

        product /= n_i;

        vector<Ciphertext> intermediate(product);
        Ciphertext temp;

        for (uint64_t k = 0; k < product; k++) {
            evaluator_->multiply_plain_ntt(expanded_query[0], (*cur)[k], intermediate[k]);

            for (uint64_t j = 1; j < n_i; j++) {
                evaluator_->multiply_plain_ntt(expanded_query[j], (*cur)[k + j * product], temp);
                evaluator_->add(intermediate[k],
                                temp); // Adds to first component.
            }
        }

        for (uint32_t jj = 0; jj < intermediate.size(); jj++) {
            evaluator_->transform_from_ntt(intermediate[jj]);
        }

        if (i == nvec.size() - 1) {
            return intermediate;
        } else {
            intermediate_plain.clear();
            intermediate_plain.reserve(pir_params_.expansion_ratio * product);
            cur = &intermediate_plain;

            util::Pointer tempplain_ptr(allocate_zero_poly(
                pir_params_.expansion_ratio * product, coeff_count, my_pool));

            for (uint64_t rr = 0; rr < product; rr++) {

                decompose_to_plaintexts_ptr(intermediate[rr],
                                            tempplain_ptr.get() +
                                                rr * pir_params_.expansion_ratio * coeff_count);

                for (uint32_t jj = 0; jj < pir_params_.expansion_ratio; jj++) {
                    int offset = rr * pir_params_.expansion_ratio * coeff_count + jj * coeff_count;
                    intermediate_plain.emplace_back(coeff_count, tempplain_ptr.get() + offset);
                }
            }

            product *= pir_params_.expansion_ratio; // multiply by expansion rate.
        }
    }

    // This should never get here
    assert(0);
    vector<Ciphertext> fail(1);
    return fail;
}

inline vector<Ciphertext> PIRServer::expand_query(const Ciphertext &encrypted, uint32_t m,
                                           uint32_t client_id) {

#ifdef DEBUG
    uint64_t plainMod = expanded_params_.plain_modulus().value();
    cout << "PIRServer side plain modulus = " << plainMod << endl;
#endif

    GaloisKeys &galkey = galoisKeys_[client_id];

    // Assume that m is a power of 2. If not, round it to the next power of 2.
    uint32_t logm = ceil(log2(m));
    Plaintext two("2");

    vector<int> galois_elts;
    int n = expanded_params_.poly_modulus().coeff_count() - 1;

    for (uint32_t i = 0; i < logm; i++) {
        galois_elts.push_back((n + exponentiate_uint64(2, i)) / exponentiate_uint64(2, i));
    }

    vector<Ciphertext> temp;
    temp.push_back(encrypted);
    Ciphertext tempctxt;
    Ciphertext tempctxt_rotated;
    Ciphertext tempctxt_shifted;
    Ciphertext tempctxt_rotatedshifted;

    for (uint32_t i = 0; i < logm - 1; i++) {
        vector<Ciphertext> newtemp(temp.size() << 1);
        // temp[a] = (j0 = a (mod 2**i) ? ) : Enc(x^{j0 - a}) else Enc(0).  With
        // some scaling....
        int index_raw = (n << 1) - (1 << i);
        int index = (index_raw * galois_elts[i]) % (n << 1);

        for (uint32_t a = 0; a < temp.size(); a++) {

            evaluator_->apply_galois(temp[a], galois_elts[i], galkey, tempctxt_rotated);
            evaluator_->add(temp[a], tempctxt_rotated, newtemp[a]);
            multiply_power_of_X(temp[a], tempctxt_shifted, index_raw);
            multiply_power_of_X(tempctxt_rotated, tempctxt_rotatedshifted, index);
            // Enc(2^i x^j) if j = 0 (mod 2**i).
            evaluator_->add(tempctxt_shifted, tempctxt_rotatedshifted, newtemp[a + temp.size()]);
        }
        temp = newtemp;
    }

    // Last step of the loop
    vector<Ciphertext> newtemp(temp.size() << 1);
    int index_raw = (n << 1) - (1 << (logm - 1));
    int index = (index_raw * galois_elts[logm - 1]) % (n << 1);
    for (uint32_t a = 0; a < temp.size(); a++) {
        if (a >= (m - (1 << (logm - 1)))) {                       // corner case.
            evaluator_->multiply_plain(temp[a], two, newtemp[a]); // plain multiplication by 2.
        } else {
            evaluator_->apply_galois(temp[a], galois_elts[logm - 1], galkey, tempctxt_rotated);
            evaluator_->add(temp[a], tempctxt_rotated, newtemp[a]);
            multiply_power_of_X(temp[a], tempctxt_shifted, index_raw);
            multiply_power_of_X(tempctxt_rotated, tempctxt_rotatedshifted, index);
            evaluator_->add(tempctxt_shifted, tempctxt_rotatedshifted, newtemp[a + temp.size()]);
        }
    }

    vector<Ciphertext>::const_iterator first = newtemp.begin();
    vector<Ciphertext>::const_iterator last = newtemp.begin() + m;
    vector<Ciphertext> newVec(first, last);
    return newVec;
}

inline void PIRServer::multiply_power_of_X(const Ciphertext &encrypted, Ciphertext &destination,
                                    uint32_t index) {

    int coeff_mod_count = expanded_params_.coeff_modulus().size();
    int coeff_count = expanded_params_.poly_modulus().coeff_count();
    int encrypted_count = encrypted.size();

    // First copy over.
    destination = encrypted;

    // Prepare for destination
    // Multiply X^index for each ciphertext polynomial
    for (int i = 0; i < encrypted_count; i++) {
        for (int j = 0; j < coeff_mod_count; j++) {
            negacyclic_shift_poly_coeffmod(encrypted.pointer(i) + (j * coeff_count),
                                           coeff_count - 1, index,
                                           expanded_params_.coeff_modulus()[j],
                                           destination.mutable_pointer(i) + (j * coeff_count));
        }
    }
}

inline void PIRServer::decompose_to_plaintexts_ptr(const Ciphertext &encrypted, uint64_t *plain_ptr) {

    vector<Plaintext> result;
    int coeff_count = expanded_params_.poly_modulus().coeff_count();
    int coeff_mod_count = expanded_params_.coeff_modulus().size();
    int encrypted_count = encrypted.size();

    // Generate powers of t.
    uint64_t plainModMinusOne = expanded_params_.plain_modulus().value() - 1;
    int exp = ceil(log2(plainModMinusOne + 1));

    // A triple for loop. Going over polys, moduli, and decomposed index.

    for (int i = 0; i < encrypted_count; i++) {
        const uint64_t *encrypted_pointer = encrypted.pointer(i);
        for (int j = 0; j < coeff_mod_count; j++) {
            // populate one poly at a time.
            // create a polynomial to store the current decomposition value
            // which will be copied into the array to populate it at the current
            // index.
            int logqj = log2(expanded_params_.coeff_modulus()[j].value());
            int expansion_ratio = ceil(logqj + exp - 1) / exp;

            // cout << "expansion ratio = " << expansion_ratio << endl;
            uint64_t curexp = 0;
            for (int k = 0; k < expansion_ratio; k++) {
                // Decompose here
                for (int m = 0; m < coeff_count; m++) {
                    *plain_ptr =
                        (*(encrypted_pointer + m + (j * coeff_count)) >> curexp) & plainModMinusOne;
                    plain_ptr++;
                }
                curexp += exp;
            }
        }
    }
}

vector<Plaintext> PIRServer::decompose_to_plaintexts(const Ciphertext &encrypted) {
    vector<Plaintext> result;
    int coeff_count = expanded_params_.poly_modulus().coeff_count();
    int coeff_mod_count = expanded_params_.coeff_modulus().size();
    int plain_bit_count = expanded_params_.plain_modulus().bit_count();
    int encrypted_count = encrypted.size();

    // Generate powers of t.
    uint64_t plainMod = expanded_params_.plain_modulus().value();

    // A triple for loop. Going over polys, moduli, and decomposed index.
    for (int i = 0; i < encrypted_count; i++) {
        const uint64_t *encrypted_pointer = encrypted.pointer(i);
        for (int j = 0; j < coeff_mod_count; j++) {
            // populate one poly at a time.
            // create a polynomial to store the current decomposition value
            // which will be copied into the array to populate it at the current
            // index.
            int logqj = log2(expanded_params_.coeff_modulus()[j].value());
            int expansion_ratio = ceil(logqj / log2(plainMod));

            // cout << "expansion ratio = " << expansion_ratio << endl;
            uint64_t cur = 1;
            for (int k = 0; k < expansion_ratio; k++) {
                // Decompose here
                BigPoly temp;
                temp.resize(coeff_count, plain_bit_count);
                temp.set_zero();
                uint64_t *plain_coeff = temp.pointer();
                for (int m = 0; m < coeff_count; m++) {
                    *(plain_coeff + m) =
                        (*(encrypted_pointer + m + (j * coeff_count)) / cur) % plainMod;
                }

                result.push_back(Plaintext(temp));
                cur *= plainMod;
            }
        }
    }

    return result;
}
