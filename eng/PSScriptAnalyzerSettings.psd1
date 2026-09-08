@{
    # PowerShell 7 reads UTF-8 without a BOM, including Chinese strings.
    # This rule targets Windows PowerShell's legacy default encoding.
    ExcludeRules = @('PSUseBOMForUnicodeEncodedFile')
    Rules = @{
        PSUseConsistentIndentation = @{ Enable = $true; Kind = 'space'; IndentationSize = 4 }
        PSUseConsistentWhitespace = @{ Enable = $true }
    }
}
