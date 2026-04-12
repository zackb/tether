//
//  TetherMessage.swift
//  Tether
//
//  Codable models matching the tetherd newline-delimited JSON protocol.
//

import Foundation

// MARK: - Command Enum

/// All known command types in the Tether protocol.
enum TetherCommand: String, Codable, Sendable {
    // Client → Daemon
    case clipboardSet = "clipboard_set"
    case clipboardGet = "clipboard_get"
    case fileStart = "file_start"
    case fileChunk = "file_chunk"
    case fileEnd = "file_end"
    case pairRequest = "pair_request"

    // Daemon → Client
    case clipboardUpdated = "clipboard_updated"
    case clipboardContent = "clipboard_content"
    case fileStatus = "file_status"
    case pairPending = "pair_pending"
    case error = "error"
}

// MARK: - Message

/// A single Tether protocol message.
///
/// Uses a flat structure with optional fields — the daemon's JSON payloads
/// share a unified shape where only the relevant fields are present
/// for each command type.
struct TetherMessage: Codable, Sendable {
    let command: String

    // Clipboard
    var content: String?

    // File transfer
    var filename: String?
    var size: Int64?
    var transferId: String?
    var chunkIndex: Int?
    var data: String? // Base64 encoded chunk

    // Pairing
    var deviceName: String?

    // Status / Error
    var status: String?
    var message: String?

    enum CodingKeys: String, CodingKey {
        case command, content, filename, size
        case transferId = "transfer_id"
        case chunkIndex = "chunk_index"
        case data
        case deviceName = "device_name"
        case status, message
    }
}

// MARK: - Convenience Initializers

extension TetherMessage {
    /// Create a `clipboard_set` message.
    static func clipboardSet(_ text: String) -> TetherMessage {
        TetherMessage(command: TetherCommand.clipboardSet.rawValue, content: text)
    }

    /// Create a `clipboard_get` request.
    static func clipboardGet() -> TetherMessage {
        TetherMessage(command: TetherCommand.clipboardGet.rawValue)
    }

    /// Create a `pair_request` message.
    static func pairRequest(deviceName: String) -> TetherMessage {
        TetherMessage(command: TetherCommand.pairRequest.rawValue, deviceName: deviceName)
    }

    /// Create a `file_start` message.
    static func fileStart(filename: String, size: Int64, transferId: String) -> TetherMessage {
        TetherMessage(
            command: TetherCommand.fileStart.rawValue,
            filename: filename,
            size: size,
            transferId: transferId
        )
    }

    /// Create a `file_chunk` message.
    static func fileChunk(transferId: String, chunkIndex: Int, data: String) -> TetherMessage {
        TetherMessage(
            command: TetherCommand.fileChunk.rawValue,
            transferId: transferId,
            chunkIndex: chunkIndex,
            data: data
        )
    }

    /// Create a `file_end` message.
    static func fileEnd(transferId: String) -> TetherMessage {
        TetherMessage(command: TetherCommand.fileEnd.rawValue, transferId: transferId)
    }

    /// The parsed command enum, if recognized.
    var parsedCommand: TetherCommand? {
        TetherCommand(rawValue: command)
    }
}
