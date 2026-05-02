//
//  TetherProtocol.swift
//  TetherFramework
//
//  Newline-delimited JSON framing over raw byte streams.
//

import Foundation

// Handles encoding and decoding of `TetherMessage` values using the
// newline-delimited JSON wire format spoken by `tetherd`.
public enum TetherProtocol {
    private static let encoder: JSONEncoder = {
        let e = JSONEncoder()
        e.keyEncodingStrategy = .useDefaultKeys
        return e
    }()

    private static let decoder: JSONDecoder = {
        let d = JSONDecoder()
        d.keyDecodingStrategy = .useDefaultKeys
        return d
    }()

    private static let newline = UInt8(ascii: "\n")

    // MARK: - Encoding

    // Encode a message to its wire representation (JSON + trailing newline).
    public static func encode(_ message: TetherMessage) throws -> Data {
        var data = try encoder.encode(message)
        data.append(newline)
        return data
    }

    // MARK: - Decoding

    // Scan a mutable buffer for complete newline-delimited messages.
    //
    // Parsed messages are returned and their bytes are consumed from `buffer`.
    // Any trailing partial line is left in `buffer` for the next call.
    //
    // Lines that fail JSON parsing (e.g. the daemon's bare `OK\n` response)
    // are silently skipped.
    public static func decode(buffer: inout Data) -> [TetherMessage] {
        var messages: [TetherMessage] = []

        while let newlineIndex = buffer.firstIndex(of: newline) {
            let lineData = buffer[buffer.startIndex..<newlineIndex]
            buffer = Data(buffer[buffer.index(after: newlineIndex)...])

            guard !lineData.isEmpty else { continue }

            do {
                let message = try decoder.decode(TetherMessage.self, from: Data(lineData))
                messages.append(message)
            } catch {
                // Non-JSON lines (like "OK") are silently ignored.
            }
        }

        return messages
    }
}
