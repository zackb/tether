//
//  CertificateManager.swift
//  Tether
//
//  Self-signed X.509 certificate generation, Keychain identity management,
//  and SHA-256 fingerprint computation — all in pure Swift (no OpenSSL).
//

import Foundation
import Security
import CryptoKit
import UIKit

// Manages the app's TLS identity (self-signed RSA cert + private key)
// and the set of known remote host fingerprints.
@Observable
final class CertificateManager {
    private static let keyTag = "net.jeedup.Tether.key"
    private static let certLabel = "net.jeedup.Tether.cert"
    private static let knownHostsKey = "TetherKnownHosts"
    private static let localDeviceNameKey = "TetherLocalDeviceName"

    // SHA-256 fingerprint of our own certificate (lowercase hex, no separators).
    private(set) var myFingerprint: String = ""

    // Map of known host fingerprints → device name.
    private(set) var knownHosts: [String: String] = [:]

    // Name of this device as presented to others.
    var localDeviceName: String = "" {
        didSet {
            UserDefaults.standard.set(localDeviceName, forKey: Self.localDeviceNameKey)
        }
    }

    // The `SecIdentity` used for mTLS client authentication.
    private var identity: SecIdentity?

    // MARK: - Initialization

    // Load or create the TLS identity and populate known hosts.
    func initialize() {
        loadKnownHosts()
        loadLocalDeviceName()

        if let existing = loadIdentityFromKeychain() {
            identity = existing
            myFingerprint = fingerprintFromIdentity(existing) ?? ""
            return
        }

        // First launch — generate a new self-signed identity.
        guard let newIdentity = generateAndStoreIdentity() else {
            print("CertificateManager: Failed to generate TLS identity")
            return
        }
        identity = newIdentity
        myFingerprint = fingerprintFromIdentity(newIdentity) ?? ""
    }

    // Returns the `SecIdentity` for use with `Network.framework` TLS options.
    func getIdentity() -> SecIdentity? {
        identity
    }

    // MARK: - Known Hosts

    func isHostKnown(_ fingerprint: String) -> Bool {
        knownHosts[fingerprint] != nil
    }

    func addKnownHost(fingerprint: String, name: String) {
        knownHosts[fingerprint] = name
        saveKnownHosts()
    }

    func removeKnownHost(fingerprint: String) {
        knownHosts.removeValue(forKey: fingerprint)
        saveKnownHosts()
    }

    // MARK: - Fingerprint

    // Compute the SHA-256 fingerprint of a DER-encoded certificate.
    // Output matches the daemon's `generate_fingerprint`: lowercase hex, no separators.
    static func fingerprint(ofCertificate cert: SecCertificate) -> String {
        let der = SecCertificateCopyData(cert) as Data
        let hash = SHA256.hash(data: der)
        return hash.map { String(format: "%02x", $0) }.joined()
    }

    // MARK: - Private — Keychain

    private func loadIdentityFromKeychain() -> SecIdentity? {
        // First check if the private key exists
        let keyQuery: [String: Any] = [
            kSecClass as String: kSecClassKey,
            kSecAttrApplicationTag as String: Self.keyTag.data(using: .utf8)!,
            kSecAttrKeyType as String: kSecAttrKeyTypeRSA,
            kSecReturnRef as String: true,
        ]
        var keyResult: CFTypeRef?
        guard SecItemCopyMatching(keyQuery as CFDictionary, &keyResult) == errSecSuccess else {
            return nil
        }

        // Now look for the identity (cert + key pair)
        let identityQuery: [String: Any] = [
            kSecClass as String: kSecClassIdentity,
            kSecAttrApplicationTag as String: Self.keyTag.data(using: .utf8)!,
            kSecReturnRef as String: true,
        ]
        var identityResult: CFTypeRef?
        guard SecItemCopyMatching(identityQuery as CFDictionary, &identityResult) == errSecSuccess else {
            return nil
        }
        return (identityResult as! SecIdentity)
    }

    private func fingerprintFromIdentity(_ identity: SecIdentity) -> String? {
        var cert: SecCertificate?
        guard SecIdentityCopyCertificate(identity, &cert) == errSecSuccess, let cert else {
            return nil
        }
        return Self.fingerprint(ofCertificate: cert)
    }

    private func loadKnownHosts() {
        if let data = UserDefaults.standard.data(forKey: Self.knownHostsKey),
           let hosts = try? JSONDecoder().decode([String: String].self, from: data)
        {
            knownHosts = hosts
        }
    }

    private func saveKnownHosts() {
        if let data = try? JSONEncoder().encode(knownHosts) {
            UserDefaults.standard.set(data, forKey: Self.knownHostsKey)
        }
    }

    private func loadLocalDeviceName() {
        if let name = UserDefaults.standard.string(forKey: Self.localDeviceNameKey) {
            localDeviceName = name
        } else {
            // Default name
            #if canImport(UIKit)
            localDeviceName = UIDevice.current.name
            #else
            localDeviceName = Host.current().localizedName ?? "Mac"
            #endif
        }
    }

    // MARK: - Private — Certificate Generation

    // Generate a 2048-bit RSA keypair + self-signed X.509v3 certificate and
    // store both in the Keychain as a `SecIdentity`.
    private func generateAndStoreIdentity() -> SecIdentity? {
        // 1. Generate RSA 2048 keypair in Keychain
        let keyAttrs: [String: Any] = [
            kSecAttrKeyType as String: kSecAttrKeyTypeRSA,
            kSecAttrKeySizeInBits as String: 2048,
            kSecPrivateKeyAttrs as String: [
                kSecAttrIsPermanent as String: true,
                kSecAttrApplicationTag as String: Self.keyTag.data(using: .utf8)!,
                kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlock,
            ] as [String: Any],
        ]

        var error: Unmanaged<CFError>?
        guard let privateKey = SecKeyCreateRandomKey(keyAttrs as CFDictionary, &error) else {
            print("CertificateManager: Key generation failed: \(error!.takeRetainedValue())")
            return nil
        }

        guard let publicKey = SecKeyCopyPublicKey(privateKey) else {
            print("CertificateManager: Failed to derive public key")
            return nil
        }

        // 2. Get public key PKCS#1 DER
        guard let pubKeyDER = SecKeyCopyExternalRepresentation(publicKey, nil) as Data? else {
            print("CertificateManager: Failed to export public key")
            return nil
        }

        // 3. Build self-signed X.509v3 certificate
        guard let certDER = buildSelfSignedCertificate(
            publicKeyDER: pubKeyDER,
            privateKey: privateKey,
            commonName: "Tether-iOS"
        ) else {
            print("CertificateManager: Failed to build certificate")
            return nil
        }

        // 4. Import certificate into Keychain
        guard let certificate = SecCertificateCreateWithData(nil, certDER as CFData) else {
            print("CertificateManager: Failed to create SecCertificate from DER")
            return nil
        }

        let addCertQuery: [String: Any] = [
            kSecClass as String: kSecClassCertificate,
            kSecValueRef as String: certificate,
            kSecAttrLabel as String: Self.certLabel,
            kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlock,
        ]
        let addStatus = SecItemAdd(addCertQuery as CFDictionary, nil)
        guard addStatus == errSecSuccess || addStatus == errSecDuplicateItem else {
            print("CertificateManager: Failed to add certificate to keychain: \(addStatus)")
            return nil
        }

        // 5. Retrieve the paired identity
        return loadIdentityFromKeychain()
    }

    // MARK: - ASN.1 DER Construction

    // Build a minimal self-signed X.509v3 certificate in DER format.
    private func buildSelfSignedCertificate(
        publicKeyDER: Data,
        privateKey: SecKey,
        commonName: String
    ) -> Data? {
        let now = Date()
        let tenYears = now.addingTimeInterval(315_360_000) // 10 years in seconds

        // OIDs
        let oidSHA256RSA: [UInt8] = [0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B]
        let oidRSA: [UInt8] = [0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01]
        let oidCN: [UInt8] = [0x55, 0x04, 0x03]

        // AlgorithmIdentifier: sha256WithRSAEncryption
        let sigAlgId = asn1Sequence(asn1OID(oidSHA256RSA) + asn1Null())

        // Name: CN=commonName
        let nameAttr = asn1Sequence(asn1OID(oidCN) + asn1UTF8String(commonName))
        let nameRDN = asn1Set(nameAttr)
        let name = asn1Sequence(nameRDN)

        // Validity
        let validity = asn1Sequence(asn1UTCTime(now) + asn1UTCTime(tenYears))

        // SubjectPublicKeyInfo
        let spkiAlg = asn1Sequence(asn1OID(oidRSA) + asn1Null())
        let spki = asn1Sequence(spkiAlg + asn1BitString(Array(publicKeyDER)))

        // Serial number (random 8 bytes)
        var serialBytes = [UInt8](repeating: 0, count: 8)
        _ = SecRandomCopyBytes(kSecRandomDefault, serialBytes.count, &serialBytes)
        serialBytes[0] &= 0x7F // Ensure positive

        // TBSCertificate
        let version = asn1ExplicitTag(0, asn1Integer([0x02])) // v3
        let serial = asn1Integer(serialBytes)

        let tbsContents = version + serial + sigAlgId + name + validity + name + spki
        let tbsDER = asn1Sequence(tbsContents)

        // Sign the TBSCertificate with the private key
        var signError: Unmanaged<CFError>?
        guard let signatureData = SecKeyCreateSignature(
            privateKey,
            .rsaSignatureMessagePKCS1v15SHA256,
            Data(tbsDER) as CFData,
            &signError
        ) as Data? else {
            print("CertificateManager: Signing failed: \(signError!.takeRetainedValue())")
            return nil
        }

        // Final Certificate structure
        let certContents = tbsDER + sigAlgId + asn1BitString(Array(signatureData))
        let certDER = asn1Sequence(certContents)

        return Data(certDER)
    }

    // MARK: - ASN.1 Primitives

    private func asn1Length(_ length: Int) -> [UInt8] {
        if length < 128 {
            return [UInt8(length)]
        } else if length < 256 {
            return [0x81, UInt8(length)]
        } else if length < 65536 {
            return [0x82, UInt8(length >> 8), UInt8(length & 0xFF)]
        } else {
            return [0x83, UInt8(length >> 16), UInt8((length >> 8) & 0xFF), UInt8(length & 0xFF)]
        }
    }

    private func asn1Sequence(_ contents: [UInt8]) -> [UInt8] {
        [0x30] + asn1Length(contents.count) + contents
    }

    private func asn1Set(_ contents: [UInt8]) -> [UInt8] {
        [0x31] + asn1Length(contents.count) + contents
    }

    private func asn1Integer(_ value: [UInt8]) -> [UInt8] {
        var bytes = value
        // Ensure the integer is positive (leading bit must be 0)
        if let first = bytes.first, first & 0x80 != 0 {
            bytes.insert(0x00, at: 0)
        }
        return [0x02] + asn1Length(bytes.count) + bytes
    }

    private func asn1BitString(_ contents: [UInt8]) -> [UInt8] {
        // Prepend the "unused bits" count (always 0 for byte-aligned data)
        let payload = [0x00] + contents
        return [0x03] + asn1Length(payload.count) + payload
    }

    private func asn1Null() -> [UInt8] {
        [0x05, 0x00]
    }

    private func asn1OID(_ oid: [UInt8]) -> [UInt8] {
        [0x06, UInt8(oid.count)] + oid
    }

    private func asn1UTF8String(_ string: String) -> [UInt8] {
        let bytes = Array(string.utf8)
        return [0x0C] + asn1Length(bytes.count) + bytes
    }

    private func asn1UTCTime(_ date: Date) -> [UInt8] {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyMMddHHmmss'Z'"
        formatter.timeZone = TimeZone(identifier: "UTC")
        let bytes = Array(formatter.string(from: date).utf8)
        return [0x17, UInt8(bytes.count)] + bytes
    }

    private func asn1ExplicitTag(_ tag: UInt8, _ contents: [UInt8]) -> [UInt8] {
        let tagByte = 0xA0 | tag
        return [tagByte] + asn1Length(contents.count) + contents
    }
}
