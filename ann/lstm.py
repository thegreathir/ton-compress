"""
train_lstm.py

Train an LSTM on bit-level data from base64-encoded bodies stored in JSON,
select the best model based on validation loss, and export TorchScript
for C++ inference.

Usage:
  python train_lstm.py --json_path data.json

Make sure you have:
  pip install torch torchvision
"""

import argparse
import json
import base64
import torch
import torch.nn as nn
from torch.utils.data import Dataset, DataLoader, random_split


# =========================
# 1. Dataset Class
# =========================
class Base64BitsDataset(Dataset):
    """
    A dataset that:
      1. Reads a JSON file where each entry has {"body": "base64-string"}
      2. Decodes the base64 body into bytes
      3. Converts bytes to a list of bits (0 or 1)
      4. Splits the bit list into overlapping sequences of length (seq_length+1).
         -> The first seq_length bits are the input, and the *next* bit is the target.
    """
    def __init__(self, json_path, seq_length=128):
        super().__init__()
        self.seq_length = seq_length
        
        # Load JSON
        with open(json_path, 'r') as f:
            data = json.load(f)
        
        # Convert each base64-encoded body into a list of bits
        all_bit_sequences = []
        for item in data:
            if "body" not in item:
                # Skip malformed entries
                continue
            b64body = item["body"]
            # Decode from base64 into raw bytes
            body_bytes = base64.b64decode(b64body)
            
            # Convert each byte to 8 bits
            bits = []
            for byte in body_bytes:
                for i in range(8):
                    bit = (byte >> (7 - i)) & 1
                    bits.append(bit)
            
            # Only add non-empty bit sequences
            if len(bits) > 0:
                all_bit_sequences.append(bits)
        
        # Build a list of (seq_length+1)-long chunks
        self.samples = []
        for bits in all_bit_sequences:
            # We create all possible consecutive sequences of length (seq_length+1)
            for start_idx in range(0, len(bits) - seq_length):
                chunk = bits[start_idx : start_idx + seq_length + 1]
                self.samples.append(chunk)
    
    def __len__(self):
        return len(self.samples)
    
    def __getitem__(self, idx):
        """
        Returns:
          x: the first seq_length bits as input  (shape: [seq_length])
          y: the next seq_length bits as target (shape: [seq_length])
        """
        chunk = self.samples[idx]
        x = chunk[:-1]  # all but last
        y = chunk[1:]   # all but first
        x_tensor = torch.tensor(x, dtype=torch.float32)
        y_tensor = torch.tensor(y, dtype=torch.float32)
        return x_tensor, y_tensor


# =========================
# 2. LSTM Model
# =========================
class BitLSTM(nn.Module):
    """
    A simple LSTM model that predicts the next bit (0/1).
    - Input size = 1 (since we feed a single bit at each time step)
    - Hidden size = user-defined
    - Output size = 1 (logit for next bit)
    """
    def __init__(self, hidden_size=128, num_layers=1):
        super().__init__()
        self.hidden_size = hidden_size
        self.num_layers = num_layers
        
        # LSTM: input shape is (batch, seq_len, input_size)
        self.lstm = nn.LSTM(
            input_size=1,
            hidden_size=hidden_size,
            num_layers=num_layers,
            batch_first=True
        )
        
        # Final linear layer to map LSTM hidden state -> 1-dimensional output
        self.fc = nn.Linear(hidden_size, 1)
    
    def forward(self, x):
        """
        Args:
          x: shape (batch_size, seq_length, 1)
        Returns:
          logits: shape (batch_size, seq_length, 1)
        """
        lstm_out, _ = self.lstm(x)  # (batch, seq_length, hidden_size)
        logits = self.fc(lstm_out)  # (batch, seq_length, 1)
        return logits


# =========================
# 3. Training & Evaluation
# =========================
def train_one_epoch(model, dataloader, criterion, optimizer, device="cpu"):
    model.train()
    total_loss = 0.0
    for x, y in dataloader:
        x = x.unsqueeze(-1).to(device)  # shape: (batch, seq_length, 1)
        y = y.unsqueeze(-1).to(device)  # shape: (batch, seq_length, 1)
        
        optimizer.zero_grad()
        logits = model(x)  # (batch, seq_length, 1)
        loss = criterion(logits, y)
        loss.backward()
        optimizer.step()
        
        total_loss += loss.item()
    
    return total_loss / len(dataloader)


def evaluate(model, dataloader, criterion, device="cpu"):
    model.eval()
    total_loss = 0.0
    with torch.no_grad():
        for x, y in dataloader:
            x = x.unsqueeze(-1).to(device)
            y = y.unsqueeze(-1).to(device)
            
            logits = model(x)
            loss = criterion(logits, y)
            total_loss += loss.item()
    return total_loss / len(dataloader)


# =========================
# 4. Main Script
# =========================
def main(args):
    # Hyperparameters
    json_path = args.json_path
    seq_length = 128
    batch_size = 32
    hidden_size = 128
    num_layers = 1
    num_epochs = 5
    lr = 1e-3
    device = "cuda" if torch.cuda.is_available() else "cpu"
    
    # 4.1 Create dataset & dataloader
    dataset = Base64BitsDataset(json_path, seq_length=seq_length)
    if len(dataset) == 0:
        print("Dataset is empty. Exiting...")
        return
    
    # 4.2 Split dataset into train/val
    val_ratio = 0.1
    val_size = int(len(dataset) * val_ratio)
    train_size = len(dataset) - val_size
    train_dataset, val_dataset = random_split(dataset, [train_size, val_size])
    
    train_loader = DataLoader(train_dataset, batch_size=batch_size, shuffle=True)
    val_loader = DataLoader(val_dataset, batch_size=batch_size, shuffle=False)
    
    # 4.3 Instantiate model, loss, optimizer
    model = BitLSTM(hidden_size=hidden_size, num_layers=num_layers).to(device)
    criterion = nn.BCEWithLogitsLoss()
    optimizer = torch.optim.Adam(model.parameters(), lr=lr)
    
    # 4.4 Track best model
    best_val_loss = float('inf')
    best_checkpoint_path = "best_bit_lstm_model.pth"
    
    for epoch in range(num_epochs):
        train_loss = train_one_epoch(model, train_loader, criterion, optimizer, device=device)
        val_loss = evaluate(model, val_loader, criterion, device=device)
        
        print(f"Epoch {epoch+1}/{num_epochs} | "
              f"Train Loss: {train_loss:.4f} | Val Loss: {val_loss:.4f}")
        
        # Check if this is the best model so far
        if val_loss < best_val_loss:
            best_val_loss = val_loss
            torch.save(model.state_dict(), best_checkpoint_path)
            print(f"  [*] New best model saved at epoch {epoch+1} with val_loss={val_loss:.4f}")
    
    print(f"Training complete. Best validation loss: {best_val_loss:.4f}")
    print(f"Best model is stored at '{best_checkpoint_path}'")
    
    # 4.5 Convert best model to TorchScript & Save
    #     - first load the best checkpoint, then script/trace the model, and save
    model.load_state_dict(torch.load(best_checkpoint_path, map_location=device))
    model.eval()
    
    # We must feed an example of the correct shape to trace or script
    # Example input shape: [batch=1, seq_len=seq_length, input_size=1]
    example_input = torch.zeros((1, seq_length, 1), dtype=torch.float32).to(device)
    
    # We'll use script rather than trace to handle dynamic LSTM internals
    scripted_model = torch.jit.script(model, example_inputs=(example_input,))
    
    # Save the TorchScript model
    torchscript_path = "best_bit_lstm_model_jit.pt"
    scripted_model.save(torchscript_path)
    print(f"Saved TorchScript model to: {torchscript_path}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--json_path", type=str, required=True,
                        help="Path to the JSON file containing base64-encoded bodies.")
    args = parser.parse_args()
    
    main(args)
